#include "tjsCommHead.h"
//---------------------------------------------------------------------------
#include "tjsUtils.h"
#include "MsgIntf.h"
#include "BitmapBitsAlloc.h"
#include "SysInitIntf.h"
#include "EventIntf.h"
#include "DebugIntf.h"
#include "vita_klog.h"
#include <cstdio>

class BasicAllocator : public iTVPMemoryAllocator
{
public:
	BasicAllocator() {
		TVPAddLog( TJS_W("(info) Use malloc for Bitmap") );
	}
	void* allocate( size_t size ) { return malloc(size); }
	void free( void* mem ) { ::free( mem ); }
};
#if 0
class GlobalAllocAllocator : public iTVPMemoryAllocator
{
public:
	GlobalAllocAllocator() {
		TVPAddLog( TJS_W("(info) Use GlobalAlloc allocater for Bitmap") );
	}
	void* allocate( size_t size ) { return GlobalAlloc(GMEM_FIXED,size); }
	void free( void* mem ) { GlobalFree((HGLOBAL)mem ); }
};
class HeapAllocAllocator : public iTVPMemoryAllocator
{
	static const DWORD HeapFlag = 0;

	HANDLE HeapHandle;
public:
	HeapAllocAllocator() : HeapHandle(NULL) {
		tTJSVariant val;
		tjs_uint64 size = 0;
		if(TVPGetCommandLine(TJS_W("-bitmapheapsize"), &val)) {
			ttstr str(val);
			if(str == TJS_W("auto")) {
				size = 0;
			} else {
				size = (tjs_int64)val;
				if( size == 0 ) {
					HeapHandle = ::HeapCreate( HeapFlag, 0, 0 );
				}
				size *= 1024*1024;
			}
		}
		if( HeapHandle == NULL ) {
			if( size == 0 ) {
				MEMORYSTATUSEX status = { sizeof(MEMORYSTATUSEX) };
				::GlobalMemoryStatusEx(&status);
				size = status.ullAvailVirtual;
				if( size > (512LL*1024*1024) ) {
					size -= (128LL*1024*1024);
				} else {
					size /= 2;
				}
				if( size > (512LL*1024*1024) ) {
					size = (512LL*1024*1024); // 512MB�ɐ���
				}
			}
			while( HeapHandle == NULL && size > (1024*1024) ) {
				HeapHandle = ::HeapCreate( HeapFlag, (SIZE_T)size, 0 );
				if( HeapHandle == NULL ) {
					if( size > (128LL*1024*1024) ) {
						size -= (128LL*1024*1024);
					} else {
						size /= 2;
					}
				}
			} 
		}

		if( HeapHandle ) {
			ULONG HeapInformation = 2;
			BOOL lfhenable = ::HeapSetInformation( HeapHandle, HeapCompatibilityInformation, &HeapInformation, sizeof(HeapInformation) );
		}
		TVPAddLog( TJS_W("(info) Use separate heap allocater for Bitmap") );
	}
	virtual ~HeapAllocAllocator() {
		if( HeapHandle ) ::HeapDestroy(HeapHandle);
		HeapHandle = NULL;
	}
	void* allocate( size_t size ) {
		if( HeapHandle == NULL ) return NULL;
		void* result = ::HeapAlloc( HeapHandle, HeapFlag, size );
		if( result == NULL ) {
			::HeapCompact( HeapHandle, HeapFlag );	// try compact
			result = ::HeapAlloc( HeapHandle, HeapFlag, size ); // retry
		}
		return result;
	}
	void free( void* mem ) {
		if( HeapHandle ) {
			BOOL ret = ::HeapFree( HeapHandle, HeapFlag, mem );
			::HeapCompact( HeapHandle, HeapFlag );
		}
	}
};
#endif

iTVPMemoryAllocator* tTVPBitmapBitsAlloc::Allocator = NULL;
tTJSCriticalSection tTVPBitmapBitsAlloc::AllocCS;

// Running total of bytes currently held by live bitmap pixel data
// (mallinfo() itself pulls in enough extra newlib code to blow this
// build's SCE segment budget, confirmed once already -- tracking by
// hand instead). Re-added after the two-slot exact-size cache (f051e46)
// still hit "Cannot allocate memory" for both known hot sizes at once
// on the title screen: need fresh numbers to tell whether that's still
// fragmentation or genuine peak-usage growth this time.
static tjs_uint64 TVPTotalBitmapBytes = 0;
//
// Recycle the last couple of freed buffers instead of handing them back
// to malloc: on Free(), stash into whichever slot is empty rather than
// calling Allocator->free(); on Alloc(), reuse a slot on an exact size
// match instead of calling Allocator->allocate() at all. Two exact-size
// slots (unrolled rather than a loop over an array, to fit this build's
// SCE segment budget) already covers the two hot sizes seen on hardware
// (one full-screen resolution, plus one slightly-cropped variant) without
// the memory overhead a size-rounding scheme would add to every bitmap.
static void *TVPBitmapFreeCache0 = NULL;
static tjs_uint TVPBitmapFreeCacheBytes0 = 0;
static void *TVPBitmapFreeCache1 = NULL;
static tjs_uint TVPBitmapFreeCacheBytes1 = 0;

void tTVPBitmapBitsAlloc::InitializeAllocator() {
	if( Allocator == NULL ) {
#if 0
		tTJSVariant val;
		if (TVPGetCommandLine(TJS_W("-bitmapallocator"), &val)) {
			ttstr str(val);
			if(str == TJS_W("globalalloc"))
				Allocator = new GlobalAllocAllocator();
			else if(str == TJS_W("separateheap"))
				Allocator = new HeapAllocAllocator();
			else    // malloc
#endif
				Allocator = new BasicAllocator();
#if 0
		} else {
			Allocator = new GlobalAllocAllocator();
		}
#endif
		// Pre-seed both cache slots with the two hot sizes confirmed on
		// hardware (800x600, confirmed repeatedly at this exact size --
		// 796x593 was only ever seen once, in a different context) right
		// now, while the heap is at its most contiguous -- this is the
		// very first bitmap allocation of the whole run. One pre-seeded
		// slot for 800x600 still wasn't enough on hardware: a crossfade
		// needs the OLD and NEW image alive at once, both 800x600, so
		// both slots are pre-seeded with this size instead of splitting
		// across two different sizes.
		tjs_uint bytes0 = 16 + 1920040u + sizeof(tTVPLayerBitmapMemoryRecord) + sizeof(tjs_uint32)*2;
		TVPBitmapFreeCache0 = Allocator->allocate(bytes0);
		TVPBitmapFreeCacheBytes0 = TVPBitmapFreeCache0 ? bytes0 : 0;
		TVPBitmapFreeCache1 = Allocator->allocate(bytes0);
		TVPBitmapFreeCacheBytes1 = TVPBitmapFreeCache1 ? bytes0 : 0;
	}
}
void tTVPBitmapBitsAlloc::FreeAllocator() {
	if( Allocator ) {
		if (TVPBitmapFreeCache0) { Allocator->free(TVPBitmapFreeCache0); TVPBitmapFreeCache0 = NULL; }
		if (TVPBitmapFreeCache1) { Allocator->free(TVPBitmapFreeCache1); TVPBitmapFreeCache1 = NULL; }
		delete Allocator;
	}
	Allocator = NULL;
}
static tTVPAtExit
	TVPUninitMessageLoad(TVP_ATEXIT_PRI_CLEANUP, tTVPBitmapBitsAlloc::FreeAllocator);

void* tTVPBitmapBitsAlloc::Alloc( tjs_uint size, tjs_uint width, tjs_uint height ) {
	if(size == 0) return NULL;
	tTJSCriticalSectionHolder Lock(AllocCS);	// Lock

	InitializeAllocator();
	tjs_uint8 * ptrorg, * ptr;
	tjs_uint allocbytes = 16 + size + sizeof(tTVPLayerBitmapMemoryRecord) + sizeof(tjs_uint32)*2;

	if (TVPBitmapFreeCache0 && TVPBitmapFreeCacheBytes0 == allocbytes) {
		ptr = ptrorg = (tjs_uint8*)TVPBitmapFreeCache0;
		TVPBitmapFreeCache0 = NULL;
	} else if (TVPBitmapFreeCache1 && TVPBitmapFreeCacheBytes1 == allocbytes) {
		ptr = ptrorg = (tjs_uint8*)TVPBitmapFreeCache1;
		TVPBitmapFreeCache1 = NULL;
	} else {
		ptr = ptrorg = (tjs_uint8*)Allocator->allocate(allocbytes);
	}
	if(!ptr)
	{
		char diag[64];
		snprintf(diag, sizeof(diag), "[KK4V] Bitmap OOM, live=%uKB",
			(unsigned)(TVPTotalBitmapBytes / 1024));
		KK4V_Log(diag);
	}
	if(!ptr) TVPThrowExceptionMessage(TVPCannotAllocateBitmapBits,
		TJS_W("at TVPAllocBitmapBits"), ttstr((tjs_int)allocbytes) + TJS_W("(") +
			ttstr((int)width) + TJS_W("x") + ttstr((int)height) + TJS_W(")"));
	TVPTotalBitmapBytes += size;
	// align to a paragraph ( 16-bytes )
	ptr += 16 + sizeof(tTVPLayerBitmapMemoryRecord);
	*reinterpret_cast<tTJSPointerSizedInteger*>(&ptr) >>= 4;
	*reinterpret_cast<tTJSPointerSizedInteger*>(&ptr) <<= 4;

	tTVPLayerBitmapMemoryRecord * record =
		(tTVPLayerBitmapMemoryRecord*)
		(ptr - sizeof(tTVPLayerBitmapMemoryRecord) - sizeof(tjs_uint32));

	// fill memory allocation record
	record->alloc_ptr = (void *)ptrorg;
	record->size = size;
	record->sentinel_backup1 = rand() + (rand() << 16);
	record->sentinel_backup2 = rand() + (rand() << 16);

	// set sentinel
	*(tjs_uint32*)(ptr - sizeof(tjs_uint32)) = ~record->sentinel_backup1;
	*(tjs_uint32*)(ptr + size              ) = ~record->sentinel_backup2;
		// Stored sentinels are nagated, to avoid that the sentinel backups in
		// tTVPLayerBitmapMemoryRecord becomes the same value as the sentinels.
		// This trick will make the detection of the memory corruption easier.
		// Because on some occasions, running memory writing will write the same
		// values at first sentinel and the tTVPLayerBitmapMemoryRecord.

	// return buffer pointer
	return ptr;
}
void tTVPBitmapBitsAlloc::Free( void* ptr ) {
	if(ptr)
	{
		tTJSCriticalSectionHolder Lock(AllocCS);	// Lock

		// get memory allocation record pointer
		tjs_uint8 *bptr = (tjs_uint8*)ptr;
		tTVPLayerBitmapMemoryRecord * record =
			(tTVPLayerBitmapMemoryRecord*)
			(bptr - sizeof(tTVPLayerBitmapMemoryRecord) - sizeof(tjs_uint32));

		// check sentinel
		if(~(*(tjs_uint32*)(bptr - sizeof(tjs_uint32))) != record->sentinel_backup1)
			TVPThrowExceptionMessage( TVPLayerBitmapBufferUnderrunDetectedCheckYourDrawingCode );
		if(~(*(tjs_uint32*)(bptr + record->size      )) != record->sentinel_backup2)
			TVPThrowExceptionMessage( TVPLayerBitmapBufferOverrunDetectedCheckYourDrawingCode );

		TVPTotalBitmapBytes -= record->size;
		tjs_uint allocbytes = 16 + record->size + sizeof(tTVPLayerBitmapMemoryRecord) + sizeof(tjs_uint32)*2;
		if (!TVPBitmapFreeCache0) {
			TVPBitmapFreeCache0 = record->alloc_ptr;
			TVPBitmapFreeCacheBytes0 = allocbytes;
		} else if (!TVPBitmapFreeCache1) {
			TVPBitmapFreeCache1 = record->alloc_ptr;
			TVPBitmapFreeCacheBytes1 = allocbytes;
		} else {
			Allocator->free( record->alloc_ptr );
		}
	}
}

