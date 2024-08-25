import unqlite
import struct

def parse_thread_id_map(value):
    decoded_map = value.decode('utf-8')
    thread_map = {}
    for item in decoded_map.split(';'):
        if item:
            real_id, compressed_id = item.split(':')
            thread_map[int(compressed_id)] = int(real_id)
    return thread_map

def parse_batch_data(value):
    STRUCT_FORMAT = ">BBHHI"  # 根据你之前的10字节结构
    record_size = struct.calcsize(STRUCT_FORMAT)

    records = []
    for i in range(0, len(value), record_size):
        unpacked_data = struct.unpack(STRUCT_FORMAT, value[i:i+record_size])
        records.append(unpacked_data)
    return records

def read_unqlite_db(db_filename):
    try:
        db = unqlite.UnQLite(db_filename)
        keys = db.keys()

        for key in keys:
            print(f"Key: {key}")
            value = db[key]
            if key == "thread_id_map":
                thread_id_map = parse_thread_id_map(value)
                print("Parsed thread_id_map:", thread_id_map)
            elif key.startswith("batch_"):
                batch_data = parse_batch_data(value)
                for record in batch_data:
                    print("Parsed batch record:", record)
    except Exception as e:
        print(f"Error reading UnQLite database: {e}")
    finally:
        db.close()

if __name__ == "__main__":
    # 替换为你自己的数据库文件名
    db_filename = "dummp_worker7.db"
    read_unqlite_db(db_filename)
