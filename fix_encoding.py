import glob
import os

files = glob.glob('src/core/tjs2/*.cpp') + glob.glob('src/core/tjs2/*.h')
for f in files:
    try:
        # read as raw bytes, replace, write as raw bytes
        with open(f, 'rb') as file:
            content = file.read()
        
        # In Shift-JIS, 0x5C is yen sign, but it's used as backslash.
        # However, 0x5C is just 0x5C in bytes. Wait! If it's just 0x5C, then C++ compiler sees it as backslash!
        # The reason we saw ¥ and ‾ is because I converted with iconv!
        # Wait, if I DIDN'T convert with iconv, the compiler saw 0x5C (backslash) and 0x7E (tilde).
        # Wait, but gcc complained about ‾32 and '¥'-' '!
        # Oh! GCC natively uses UTF-8 and the terminal uses UTF-8. 
        # If the file is encoded in UTF-8 but contains actual ¥ (U+00A5) and ‾ (U+203E) characters!
        # How did it get there? Zeas2 might have committed it as UTF-8 with those characters by mistake!
        
        with open(f, 'r', encoding='utf-8') as file:
            content_str = file.read()
            
        new_content = content_str.replace('¥', '\\').replace('‾', '~').replace('stream>Write', 'stream->Write')
        
        if new_content != content_str:
            with open(f, 'w', encoding='utf-8') as file:
                file.write(new_content)
    except Exception as e:
        # Some files might be shift-jis
        pass
