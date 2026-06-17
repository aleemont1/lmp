#!/usr/bin/env python3
import argparse
import sys
import time
import threading
import os

try:
    import serial
except ImportError:
    print("Error: 'pyserial' package is not installed.")
    print("Please install pyserial: pip install pyserial")
    sys.exit(1)

# ANSI Colors
COLOR_RESET = "\033[0m"
COLOR_TX = "\033[36m"      # Cyan
COLOR_RX = "\033[33m"      # Yellow
COLOR_SYSTEM = "\033[35m"  # Magenta
COLOR_WARN = "\033[31m"    # Red (for drops/errors)

def read_serial(port: str, baud: int, prefix: str, color: str, log_file):
    try:
        ser = serial.Serial(port, baud, timeout=1.0)
        msg = f"Connected to {port} ({prefix})"
        print(f"{COLOR_SYSTEM}[MONITOR] {msg}{COLOR_RESET}")
        log_file.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] [MONITOR] {msg}\n")
        log_file.flush()
    except Exception as e:
        msg = f"Failed to connect to {port} ({prefix}): {e}"
        print(f"{COLOR_WARN}[MONITOR] {msg}{COLOR_RESET}")
        log_file.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] [MONITOR] {msg}\n")
        log_file.flush()
        return

    buffer = ""
    while True:
        try:
            data = ser.read_all()
            if data:
                buffer += data.decode('utf-8', errors='ignore')
                while '\n' in buffer:
                    line, buffer = buffer.split('\n', 1)
                    line = line.strip()
                    if not line:
                        continue
                    
                    # Color formatting for terminal console
                    if any(kw in line for kw in ["[LOSS SIMULATION]", "[SIMULATED LOSS]", "Retransmit", "Failed", "mismatch", "Error"]):
                        fmt = f"{COLOR_WARN}{line}{COLOR_RESET}"
                    else:
                        fmt = f"{color}{line}{COLOR_RESET}"
                        
                    timestamp = time.strftime('%H:%M:%S')
                    print(f"[{timestamp}] [{prefix}] {fmt}")
                    
                    # Write to the dedicated log file (no interleaving possible since it's a dedicated file!)
                    log_file.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {line}\n")
                    log_file.flush()
            time.sleep(0.01)
        except serial.SerialException as e:
            msg = f"Serial error on {port} ({prefix}): {e}"
            print(f"{COLOR_WARN}[MONITOR] {msg}{COLOR_RESET}")
            log_file.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] [MONITOR] {msg}\n")
            log_file.flush()
            break
        except Exception as e:
            msg = f"Unexpected error on {prefix}: {e}"
            print(f"{COLOR_WARN}[MONITOR] {msg}{COLOR_RESET}")
            break

    try:
        ser.close()
    except:
        pass

def main():
    parser = argparse.ArgumentParser(description="Dual-port LoRaMultiPacket ACK/NACK Hardware Test Monitor")
    parser.add_argument("-t", "--txport", default="/dev/ttyUSB1", help="Serial port of Transmitter (default: /dev/ttyUSB1)")
    parser.add_argument("-r", "--rxport", default="/dev/ttyUSB2", help="Serial port of Receiver (default: /dev/ttyUSB2)")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("-l", "--log", default="reliable_hw_test.log", help="Base name of output log files (default: reliable_hw_test.log)")
    args = parser.parse_args()

    # Split base log filename to derive separate filenames
    log_dir = os.path.dirname(args.log)
    log_base, log_ext = os.path.splitext(os.path.basename(args.log))
    
    tx_log_path = os.path.abspath(os.path.join(log_dir, f"{log_base}_tx{log_ext}"))
    rx_log_path = os.path.abspath(os.path.join(log_dir, f"{log_base}_rx{log_ext}"))
    
    print(f"{COLOR_SYSTEM}[MONITOR] Transmitter logs will be saved to: {tx_log_path}{COLOR_RESET}")
    print(f"{COLOR_SYSTEM}[MONITOR] Receiver logs will be saved to: {rx_log_path}{COLOR_RESET}")
    
    try:
        tx_log_file = open(tx_log_path, "w", encoding="utf-8")
        tx_log_file.write(f"=== LoRaMultiPacket ACK/NACK HW Test Transmitter Log Started at {time.strftime('%Y-%m-%d %H:%M:%S')} ===\n")
        tx_log_file.flush()
    except Exception as e:
        print(f"{COLOR_WARN}Error: Could not open transmitter log file: {e}{COLOR_RESET}")
        sys.exit(1)

    try:
        rx_log_file = open(rx_log_path, "w", encoding="utf-8")
        rx_log_file.write(f"=== LoRaMultiPacket ACK/NACK HW Test Receiver Log Started at {time.strftime('%Y-%m-%d %H:%M:%S')} ===\n")
        rx_log_file.flush()
    except Exception as e:
        print(f"{COLOR_WARN}Error: Could not open receiver log file: {e}{COLOR_RESET}")
        tx_log_file.close()
        sys.exit(1)

    # Launch reader threads
    t_tx = threading.Thread(target=read_serial, args=(args.txport, args.baud, "TX_BOARD", COLOR_TX, tx_log_file), daemon=True)
    t_rx = threading.Thread(target=read_serial, args=(args.rxport, args.baud, "RX_BOARD", COLOR_RX, rx_log_file), daemon=True)

    t_tx.start()
    t_rx.start()

    print(f"{COLOR_SYSTEM}[MONITOR] Monitoring started. Press Ctrl+C to stop...{COLOR_RESET}")
    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        print(f"\n{COLOR_SYSTEM}[MONITOR] Exiting and closing log files.{COLOR_RESET}")
    finally:
        tx_log_file.write(f"=== Transmitter Log Ended at {time.strftime('%Y-%m-%d %H:%M:%S')} ===\n")
        tx_log_file.close()
        rx_log_file.write(f"=== Receiver Log Ended at {time.strftime('%Y-%m-%d %H:%M:%S')} ===\n")
        rx_log_file.close()

if __name__ == "__main__":
    main()
