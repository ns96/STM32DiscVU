import struct
import os
import sys

# Constants matching C structures
# typedef struct {
#     int totalTime;
#     char hash[12];
#     char fullLine[32];
# } SideMapEntry;
SIDE_MAP_STRUCT = "<i12s32s" # 4 + 12 + 32 = 48 bytes
SIDE_MAP_SIZE = 48

# typedef struct __attribute__((packed)) {
#     char hash[11];
#     uint8_t padding;
#     uint32_t offset;
# } AudioIndexEntry;
AUDIO_IDX_STRUCT = "<11sBI" # 11 + 1 + 4 = 16 bytes
AUDIO_IDX_SIZE = 16

def convert_side_file(input_path, output_path):
    print(f"Converting {input_path} to {output_path}...")
    entries = []
    with open(input_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line: continue
            
            parts = line.split('_')
            if len(parts) < 5: continue
            
            # DCT2A_1_c1176104dd_320_320
            # parts[2] is hash, parts[4] is totalTime
            try:
                hash_str = parts[2]
                total_time = int(parts[4])
                
                # Truncate strings if necessary
                hash_bytes = hash_str.encode('ascii')[:11] + b'\x00'
                full_line_bytes = line.encode('ascii')[:31] + b'\x00'
                
                entry = struct.pack(SIDE_MAP_STRUCT, 
                                   total_time, 
                                   hash_bytes.ljust(12, b'\x00'), 
                                   full_line_bytes.ljust(32, b'\x00'))
                entries.append((total_time, entry))
            except (ValueError, IndexError):
                continue
    
    # Sort by totalTime for binary search on STM32
    entries.sort(key=lambda x: x[0])
    
    with open(output_path, 'wb') as f:
        for _, data in entries:
            f.write(data)
    print(f"Saved {len(entries)} entries ({len(entries) * SIDE_MAP_SIZE} bytes)")

def convert_audio_db(input_path, output_path):
    print(f"Converting {input_path} to {output_path} (Index only)...")
    index_entries = []
    
    current_offset = 0
    with open(input_path, 'rb') as f:
        for line_bytes in f:
            line = line_bytes.decode('ascii', errors='ignore').strip()
            if not line: 
                current_offset += len(line_bytes)
                continue
            
            # Tab separated: HASH\tDURATION\tBITRATE\tDEVICE_PATH\tSOURCE_PATH
            parts = line.split('\t')
            if len(parts) >= 1:
                hash_str = parts[0]
                if len(hash_str) >= 10:
                    hash_bytes = hash_str[:10].encode('ascii') + b'\x00'
                    entry = struct.pack(AUDIO_IDX_STRUCT, 
                                       hash_bytes, 
                                       0, # padding
                                       current_offset)
                    index_entries.append((hash_str[:10], entry))
            
            current_offset += len(line_bytes)
            
    # Sort index by hash for binary search
    index_entries.sort(key=lambda x: x[0])
    
    with open(output_path, 'wb') as f:
        for _, data in index_entries:
            f.write(data)
    print(f"Saved {len(index_entries)} index entries ({len(index_entries) * AUDIO_IDX_SIZE} bytes)")

if __name__ == "__main__":
    # Hardcoded default files
    files_to_convert = sys.argv[1:] if len(sys.argv) > 1 else ["audiodb.txt", "sideA.txt", "sideB.txt"]
    
    if not files_to_convert:
        print("Usage: python db_converter.py [file1.txt] [file2.txt] ...")
        sys.exit(1)
        
    for arg in files_to_convert:
        if not os.path.exists(arg):
            print(f"Skipping {arg} (File not found)")
            continue
            
        if 'side' in arg.lower():
            out = arg.replace('.txt', '.bin')
            convert_side_file(arg, out)
        elif 'audiodb' in arg.lower():
            out = arg.replace('.txt', '.idx')
            convert_audio_db(arg, out)
        else:
            print(f"Unknown file type: {arg}")
