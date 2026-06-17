#!/usr/bin/env python3
import argparse
import csv
import os
import re
import sys
import time
from typing import List

try:
    import serial
except ImportError:
    print("Error: 'pyserial' package is not installed.")
    print("Please run this script via: ./tools/run_logger.sh")
    sys.exit(1)

try:
    from textual.app import App, ComposeResult
    from textual.containers import Container, Horizontal, Vertical, Grid
    from textual.widgets import Header, Footer, Label, RichLog, Static
    from textual.reactive import reactive
    from textual.worker import get_current_worker
    from rich.text import Text
    from rich.markup import escape
except ImportError:
    print("Error: 'textual' package is not installed.")
    print("Please run this script via: ./tools/run_logger.sh")
    sys.exit(1)

# Regex Patterns for log parsing
RE_RX_PARSE = re.compile(r"RX PARSE: ID=(\d+) Chunk=(\d+)/(\d+) Len=(\d+)(?:\s+CRC=(0x[0-9a-fA-F]+))?")
RE_METRICS = re.compile(r"METRICS: RSSI=(-?\d+\.\d+) dBm \| SNR=(-?\d+\.\d+) dB \| Size=(\d+)")
RE_ERROR = re.compile(r"(RX Error detected|Packet Parsed Error)")
RE_VALIDATION_FAIL = re.compile(r"Validation Failed: (.*)")
RE_CRC_FAIL = re.compile(r"Expected CRC \(Calc\): (0x[0-9a-fA-F]+) vs Received: (0x[0-9a-fA-F]+)")
RE_RX_ERROR_IRQ = re.compile(r"RX Error detected \(IRQ: (0x[0-9a-fA-F]+)\)")
RE_GHOST_PACKET = re.compile(r"Ghost Packet detected \(len=(\d+)\)")
RE_SIZE_MISMATCH = re.compile(r"Size Mismatch! BufferLen: (\d+).*?Header says: (\d+)")
RE_ESP_WARN_ERROR = re.compile(r"^[WE]\s+\((\d+)\)\s+([^:]+):\s+(.*)$")
RE_NATIVE_WARN_ERROR = re.compile(r"^(ERROR|WARN)\s+\[([^\]]+)\]:\s+(.*)$")

def draw_ascii_chart(data: List[float], width: int = 45, height: int = 7, title: str = "Chart") -> str:
    """Renders a 2D ASCII bar graph with aligned Y-axis ticks and labels."""
    if not data:
        return f"[bold]{title}[/]\n\n   [italic gray]<No Data Received>[/]"
    
    # Slice to the last 'width' elements to fit the chart area
    points = data[-width:]
    
    min_v = min(points)
    max_v = max(points)
    if max_v == min_v:
        max_v += 1.0  # Avoid division by zero
        
    grid = [[" " for _ in range(len(points))] for _ in range(height)]
    
    for x, val in enumerate(points):
        # Scale value to grid height
        y = int(((val - min_v) / (max_v - min_v)) * (height - 1))
        # Invert index since row 0 is top
        y = height - 1 - y
        
        # Fill column up to target height for bar look, or draw single block for line look
        grid[y][x] = "█"
        
    lines = []
    lines.append(f"[bold cyan]{title}[/] [gray](Last {len(points)} packets)[/]")
    for y in range(height):
        row_str = "".join(grid[y])
        # Format aligned y-axis ticks
        if y == 0:
            label = f"{max_v:6.1f} ┤"
        elif y == height // 2:
            label = f"{(max_v+min_v)/2:6.1f} ┤"
        elif y == height - 1:
            label = f"{min_v:6.1f} ┤"
        else:
            label = "       │"
        lines.append(f"[gray]{label}[/][green]{row_str}[/]")
        
    # Append current state
    lines.append(f"Current: [bold yellow]{data[-1]:.1f}[/] | Min: {min(data):.1f} | Max: {max(data):.1f}")
    return "\n".join(lines)


class AsciiGraph(Static):
    """A custom widget to render and update 2D ASCII graphs reactively."""
    
    data = reactive([])
    
    def __init__(self, title: str, height: int = 7, **kwargs):
        super().__init__("<no data>", markup=True, **kwargs)
        self.title = title
        self.graph_height = height
        
    def watch_data(self, new_data: List[float]) -> None:
        chart_str = draw_ascii_chart(new_data, width=45, height=self.graph_height, title=self.title)
        self.update(chart_str)


class TelemetryApp(App):
    """Textual TUI Application for LoRaMultiPacket Experiment Analytics."""
    
    # Custom stylesheet inside the app definition
    CSS = """
    Screen {
        background: #121212;
    }
    
    #sidebar {
        width: 32;
        height: 100%;
        border-right: tall $accent;
        padding: 1 2;
        background: #181818;
    }
    
    #main-content {
        height: 100%;
    }
    
    #charts-container {
        height: 13;
        layout: grid;
        grid-size: 2 1;
        grid-gutter: 2;
        border-bottom: tall $accent;
        padding: 1;
    }
    
    AsciiGraph {
        background: #1c1c1c;
        border: solid #333333;
        padding: 1;
        height: 100%;
    }
    
    #log-container {
        height: 1fr;
        padding: 1;
    }
    
    #msg-log {
        background: #151515;
        border: solid #333333;
    }
    
    .section-title {
        text-style: bold;
        color: $accent;
        margin-bottom: 1;
    }
    
    .metric-label {
        text-style: bold;
        color: #888888;
        margin-top: 1;
    }
    
    .metric-value {
        color: #ffffff;
        background: #222222;
        padding: 0 1;
        margin-bottom: 1;
    }
    """
    
    BINDINGS = [
        ("q", "quit", "Quit"),
        ("c", "clear", "Clear Stats"),
        ("v", "toggle_verbose", "Toggle Verbose"),
    ]
    
    chunks_rx = reactive(0)
    packets_rx = reactive(0)
    packets_failed = reactive(0)
    rssi_history = reactive([])
    snr_history = reactive([])
    verbose = reactive(False)
    
    def __init__(self, port: str, baud: int, output_csv: str, verbose: bool = False, **kwargs):
        super().__init__(**kwargs)
        self.port = port
        self.baud = baud
        self.output_csv = output_csv
        self.verbose = verbose
        self.sum_rssi = 0.0
        self.sum_snr = 0.0

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        with Horizontal():
            # Left Sidebar (Metrics Panel)
            with Vertical(id="sidebar"):
                yield Label("CONNECTION INFO", classes="section-title")
                yield Label("Port:", classes="metric-label")
                yield Label(self.port, classes="metric-value")
                yield Label("Baudrate:", classes="metric-label")
                yield Label(f"{self.baud} bps", classes="metric-value")
                yield Label("CSV Log:", classes="metric-label")
                yield Label(os.path.basename(self.output_csv), classes="metric-value")
                yield Label("Verbose Mode:", classes="metric-label")
                yield Label("OFF", id="val-verbose", classes="metric-value")
                
                yield Label("STATISTICS", classes="section-title")
                yield Label("Chunks Rx:", classes="metric-label")
                yield Label("0", id="val-chunks", classes="metric-value")
                yield Label("Packets Rx (OK):", classes="metric-label")
                yield Label("0", id="val-packets", classes="metric-value")
                yield Label("Packets Failed:", classes="metric-label")
                yield Label("0", id="val-failed", classes="metric-value")
                yield Label("Packet Delivery PDR:", classes="metric-label")
                yield Label("0.00 %", id="val-pdr", classes="metric-value")
                
                yield Label("LINK QUALITY", classes="section-title")
                yield Label("Avg RSSi:", classes="metric-label")
                yield Label("0.0 dBm", id="val-rssi", classes="metric-value")
                yield Label("Avg SNR:", classes="metric-label")
                yield Label("0.0 dB", id="val-snr", classes="metric-value")
            
            # Right Panel (Graphs & Logs)
            with Vertical(id="main-content"):
                with Grid(id="charts-container"):
                    yield AsciiGraph("RSSI Signal Strength (dBm)", id="rssi-graph")
                    yield AsciiGraph("SNR Noise Margin (dB)", id="snr-graph")
                with Vertical(id="log-container"):
                    yield Label("Reassembled Packets Log", classes="section-title")
                    yield RichLog(id="msg-log", auto_scroll=True, max_lines=1000, markup=True, wrap=True)
        yield Footer()

    def on_mount(self) -> None:
        """Starts the serial port reading worker in a background thread."""
        self.title = "LMP Telemetry Analytics Dashboard"
        self.run_worker(self.serial_reader_worker, thread=True)

    def serial_reader_worker(self) -> None:
        """Background thread worker to read and log serial data."""
        worker = get_current_worker()
        try:
            ser = serial.Serial(self.port, self.baud, timeout=1.0)
            time.sleep(0.5)
            ser.reset_input_buffer()
        except Exception as e:
            self.call_from_thread(self.notify, f"Error opening serial port {self.port}: {e}", severity="error")
            return

        # Setup CSV writing
        csv_file = None
        csv_writer = None
        try:
            csv_exists = os.path.exists(self.output_csv)
            csv_file = open(self.output_csv, "a", newline="", encoding="utf-8")
            csv_writer = csv.writer(csv_file)
            if not csv_exists:
                csv_writer.writerow([
                    "Timestamp", "SystemTimeMs", "MessageID", "RSSI_dBm", "SNR_dB", "PayloadSizeBytes", "Status"
                ])
                csv_file.flush()
        except Exception as e:
            self.call_from_thread(self.notify, f"Failed to open CSV log file: {e}", severity="error")
            ser.close()
            return

        last_body = ""

        # Main polling loop
        while not worker.is_cancelled:
            try:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
            except Exception as e:
                self.call_from_thread(self.notify, f"Serial disconnect: {e}", severity="error")
                break

            if not line:
                continue

            # 1. Parse Chunk Info
            rx_match = RE_RX_PARSE.search(line)
            if rx_match:
                self.call_from_thread(self.increment_chunks)
                msg_id = rx_match.group(1)
                chunk = rx_match.group(2)
                total = rx_match.group(3)
                length = rx_match.group(4)
                crc = rx_match.group(5) or "N/A"
                self.call_from_thread(
                    self.log_verbose, 
                    f"  [bold gray]CHUNK[/] | Msg ID: {msg_id} | Chunk: {chunk}/{total} | Len: {length} B | CRC: {crc}"
                )

            # 2. Parse Validation Failures
            val_match = RE_VALIDATION_FAIL.search(line)
            if val_match:
                detail = val_match.group(1)
                self.call_from_thread(
                    self.log_verbose,
                    f"  [bold yellow]VALIDATION FAIL[/] | {detail}"
                )

            # 3. Parse CRC Mismatch
            crc_match = RE_CRC_FAIL.search(line)
            if crc_match:
                calc = crc_match.group(1)
                rec = crc_match.group(2)
                self.call_from_thread(
                    self.log_verbose,
                    f"  [bold red]CRC ERROR[/] | Calc: {calc} vs Rec: {rec}"
                )

            # 4. Parse IRQ RX Errors
            irq_match = RE_RX_ERROR_IRQ.search(line)
            if irq_match:
                irq = irq_match.group(1)
                self.call_from_thread(
                    self.log_verbose,
                    f"  [bold red]RX ERROR[/] | IRQ Flags: {irq}"
                )

            # 5. Parse Ghost Packets
            ghost_match = RE_GHOST_PACKET.search(line)
            if ghost_match:
                glen = ghost_match.group(1)
                self.call_from_thread(
                    self.log_verbose,
                    f"  [bold yellow]GHOST PACKET[/] | Len: {glen}"
                )

            # 6. Parse Size Mismatch
            size_mismatch_match = RE_SIZE_MISMATCH.search(line)
            if size_mismatch_match:
                buf_len = size_mismatch_match.group(1)
                hdr_size = size_mismatch_match.group(2)
                self.call_from_thread(
                    self.log_verbose,
                    f"  [bold red]SIZE MISMATCH[/] | Buffer implies payload size mismatch (Buffer length: {buf_len}, Header: {hdr_size})"
                )

            # 7. Generic ESP-IDF Warning/Error
            esp_log_match = RE_ESP_WARN_ERROR.search(line)
            if esp_log_match:
                level = line[0]
                tag = esp_log_match.group(2)
                msg = esp_log_match.group(3)
                color = "red" if level == "E" else "yellow"
                label = "ERROR" if level == "E" else "WARNING"
                self.call_from_thread(
                    self.log_verbose,
                    f"  [bold {color}]{label} ({tag})[/] | {msg}"
                )

            # 8. Generic Native Warning/Error
            native_log_match = RE_NATIVE_WARN_ERROR.search(line)
            if native_log_match:
                level = native_log_match.group(1)
                tag = native_log_match.group(2)
                msg = native_log_match.group(3)
                color = "red" if "ERROR" in level else "yellow"
                self.call_from_thread(
                    self.log_verbose,
                    f"  [bold {color}]{level} ({tag})[/] | {msg}"
                )

            # 9. Parse Metrics
            metrics_match = RE_METRICS.search(line)
            if metrics_match:
                rssi = float(metrics_match.group(1))
                snr = float(metrics_match.group(2))
                size = int(metrics_match.group(3))
                
                # Write to CSV
                csv_writer.writerow([
                    time.strftime("%Y-%m-%d %H:%M:%S"),
                    int(time.time() * 1000),
                    self.packets_rx + 1,  # Safe guess on index
                    rssi,
                    snr,
                    size,
                    "SUCCESS"
                ])
                csv_file.flush()
                
                self.call_from_thread(self.on_packet_success, rssi, snr, size, last_body)
                last_body = ""

            # 10. Parse Reassembly Content
            if ">>> RECONSTRUCTED MESSAGE RECEIVED:" in line:
                last_body = line.split(">>> RECONSTRUCTED MESSAGE RECEIVED:")[1].strip()

            # 11. Parse Errors
            error_match = RE_ERROR.search(line)
            if error_match:
                csv_writer.writerow([
                    time.strftime("%Y-%m-%d %H:%M:%S"),
                    int(time.time() * 1000),
                    "", "", "", "", "FAILED"
                ])
                csv_file.flush()
                self.call_from_thread(self.on_packet_failure)

        # Cleanup
        ser.close()
        csv_file.close()

    # Thread-Safe UI Update Handlers
    def increment_chunks(self) -> None:
        self.chunks_rx += 1

    def on_packet_success(self, rssi: float, snr: float, size: int, body: str) -> None:
        self.packets_rx += 1
        self.sum_rssi += rssi
        self.sum_snr += snr
        self.rssi_history = self.rssi_history + [rssi]
        self.snr_history = self.snr_history + [snr]
        
        log = self.query_one("#msg-log", RichLog)
        # Parse Rich markup for styled logs in RichLog
        log.write(f"[bold green]SUCCESS[/] | Msg #{self.packets_rx} | RSSI: {rssi:.1f} dBm | SNR: {snr:.1f} dB | {size} bytes")
        if body:
            # Escape payload contents to prevent crash if payload contains bracket tags (like [ or ])
            escaped_body = escape(body)
            log.write(f"  [bold cyan]Payload:[/] {escaped_body}")

    def on_packet_failure(self) -> None:
        self.packets_failed += 1
        log = self.query_one("#msg-log", RichLog)
        log.write("[bold red]FAILED[/] | Reassembly Error or CRC Mismatch detected")

    # Watchers to trigger automatic side panel re-renderings
    def watch_chunks_rx(self) -> None:
        self.update_sidebar()

    def watch_packets_rx(self) -> None:
        self.update_sidebar()
        self.update_graphs()

    def watch_packets_failed(self) -> None:
        self.update_sidebar()

    def watch_verbose(self, value: bool) -> None:
        try:
            status = "[bold green]ON[/]" if value else "[bold red]OFF[/]"
            self.query_one("#val-verbose", Label).update(status)
        except Exception:
            pass

    def action_toggle_verbose(self) -> None:
        self.verbose = not self.verbose
        self.notify(f"Verbose mode {'enabled' if self.verbose else 'disabled'}")

    def log_verbose(self, message: str) -> None:
        if self.verbose:
            try:
                self.query_one("#msg-log", RichLog).write(message)
            except Exception:
                pass

    def update_sidebar(self) -> None:
        try:
            total_rx = self.packets_rx
            total_failed = self.packets_failed
            pdr = (total_rx / (total_rx + total_failed) * 100.0) if (total_rx + total_failed) > 0 else 0.0
            
            avg_rssi = (self.sum_rssi / total_rx) if total_rx > 0 else 0.0
            avg_snr = (self.sum_snr / total_rx) if total_rx > 0 else 0.0
            
            self.query_one("#val-chunks", Label).update(str(self.chunks_rx))
            self.query_one("#val-packets", Label).update(str(self.packets_rx))
            self.query_one("#val-failed", Label).update(str(self.packets_failed))
            self.query_one("#val-pdr", Label).update(f"{pdr:.2f} %")
            self.query_one("#val-rssi", Label).update(f"{avg_rssi:.1f} dBm")
            self.query_one("#val-snr", Label).update(f"{avg_snr:.1f} dB")
        except Exception:
            pass

    def update_graphs(self) -> None:
        try:
            self.query_one("#rssi-graph", AsciiGraph).data = self.rssi_history
            self.query_one("#snr-graph", AsciiGraph).data = self.snr_history
        except Exception:
            pass

    def action_clear(self) -> None:
        """Clears all statistics and console logs."""
        self.chunks_rx = 0
        self.packets_rx = 0
        self.packets_failed = 0
        self.sum_rssi = 0.0
        self.sum_snr = 0.0
        self.rssi_history = []
        self.snr_history = []
        try:
            self.query_one("#msg-log", RichLog).clear()
        except Exception:
            pass
        self.notify("Telemetry statistics cleared")


def main():
    parser = argparse.ArgumentParser(description="LoRaMultiPacket Interactive Telemetry Dashboard")
    parser.add_argument("-p", "--port", default="/dev/ttyUSB2", help="Serial port of the receiver board (default: /dev/ttyUSB2)")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("-o", "--output", default="experiment_results.csv", help="Output CSV filepath (default: experiment_results.csv)")
    parser.add_argument("-v", "--verbose", action="store_true", help="Enable verbose logging of individual chunks and errors")
    args = parser.parse_args()

    app = TelemetryApp(port=args.port, baud=args.baud, output_csv=args.output, verbose=args.verbose)
    app.run()


if __name__ == "__main__":
    main()
