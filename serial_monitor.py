import serial
import time

try:
    # 打开串口
    ser = serial.Serial('COM10', 115200, timeout=1)
    print("成功连接到串口 COM10")
    print("开始读取日志...")
    print("=" * 60)
    
    # 读取串口数据
    start_time = time.time()
    while time.time() - start_time < 30:  # 读取30秒日志
        if ser.in_waiting > 0:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if line:
                print(line)
        time.sleep(0.1)
    
    print("=" * 60)
    print("日志读取完成")
    ser.close()
except Exception as e:
    print(f"错误: {e}")
    if 'ser' in locals():
        ser.close()