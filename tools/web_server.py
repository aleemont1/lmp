import os
import glob
import json
import csv
import urllib.parse
from http.server import HTTPServer, BaseHTTPRequestHandler

PORT = 8500
DIRECTORY = os.path.dirname(os.path.abspath(__file__))

class DashboardHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        # Suppress standard logging to keep terminal clean unless verbose
        pass

    def do_GET(self):
        parsed_url = urllib.parse.urlparse(self.path)
        path = parsed_url.path
        query = urllib.parse.parse_qs(parsed_url.query)

        # Route 1: API - List available CSV log files
        if path == "/api/logs":
            csv_files = []
            # Search in root directory and tools/
            for file in glob.glob("*.csv") + glob.glob("tools/*.csv"):
                # Avoid duplicate listings
                name = os.path.basename(file)
                if not any(f['name'] == name for f in csv_files):
                    csv_files.append({
                        "name": name,
                        "path": file
                    })
            
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps(csv_files).encode("utf-8"))
            return

        # Route 2: API - Get data for a specific CSV file
        elif path == "/api/data":
            file_path = query.get("file", [""])[0]
            
            # Security check: resolve path and prevent directory traversal
            if not file_path or ".." in file_path:
                self.send_error(400, "Invalid file path")
                return

            if not os.path.exists(file_path):
                self.send_error(404, "File not found")
                return

            data = []
            try:
                with open(file_path, "r", encoding="utf-8") as f:
                    reader = csv.DictReader(f)
                    
                    # Normalise headers to lowercase for flexible parsing
                    fieldnames = reader.fieldnames
                    header_map = {}
                    if fieldnames:
                        for field in fieldnames:
                            header_map[field.lower()] = field
                            
                    for row in reader:
                        # Map keys safely
                        timestamp = row.get(header_map.get("timestamp", ""), "")
                        sys_time = row.get(header_map.get("systemtimems", ""), "")
                        msg_id = row.get(header_map.get("messageid", ""), "")
                        rssi = row.get(header_map.get("rssi_dbm", ""), "")
                        snr = row.get(header_map.get("snr_db", ""), "")
                        size = row.get(header_map.get("payloadsizebytes", ""), "")
                        status = row.get(header_map.get("status", ""), "")
                        
                        data.append({
                            "timestamp": timestamp,
                            "system_time": int(sys_time) if sys_time.isdigit() else None,
                            "message_id": int(msg_id) if msg_id.isdigit() else None,
                            "rssi": float(rssi) if rssi.replace('.', '', 1).replace('-', '', 1).isdigit() else None,
                            "snr": float(snr) if snr.replace('.', '', 1).replace('-', '', 1).isdigit() else None,
                            "size": int(size) if size.isdigit() else None,
                            "status": status
                        })
            except Exception as e:
                self.send_error(500, f"Error reading CSV: {e}")
                return

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(json.dumps(data).encode("utf-8"))
            return

        # Route 3: Serve static dashboard.html on root
        elif path == "/" or path == "/index.html":
            html_path = os.path.join(DIRECTORY, "dashboard.html")
            if not os.path.exists(html_path):
                self.send_error(404, "dashboard.html not found in tools/")
                return

            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            with open(html_path, "rb") as f:
                self.wfile.write(f.read())
            return

        # Route 4: Serve interactive documentation page
        elif path == "/docs" or path == "/documentation" or path == "/documentation.html":
            html_path = os.path.join(DIRECTORY, "documentation.html")
            if not os.path.exists(html_path):
                self.send_error(404, "documentation.html not found in tools/")
                return

            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            with open(html_path, "rb") as f:
                self.wfile.write(f.read())
            return

        # Route 5: API - Fetch code file contents dynamically with directory traversal security check
        elif path == "/api/file":
            file_path = query.get("path", [""])[0]
            if not file_path or ".." in file_path:
                self.send_error(400, "Invalid file path")
                return

            # Resolve absolute path and verify it is inside the project workspace directory
            workspace_dir = os.path.abspath(os.path.join(DIRECTORY, ".."))
            abs_file_path = os.path.abspath(os.path.join(workspace_dir, file_path))

            if not abs_file_path.startswith(workspace_dir):
                self.send_error(403, "Access denied")
                return

            if not os.path.exists(abs_file_path):
                self.send_error(404, "File not found")
                return

            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            with open(abs_file_path, "rb") as f:
                self.wfile.write(f.read())
            return

        # Route 6: Default fallback
        else:
            self.send_error(404, "Endpoint not found")

    def do_POST(self):
        parsed_url = urllib.parse.urlparse(self.path)
        path = parsed_url.path

        # Route 5: API - Upload CSV log file
        if path == "/api/upload":
            try:
                # Basic multipart or raw body parsing for CSV files
                content_length = int(self.headers.get('Content-Length', 0))
                file_data = self.rfile.read(content_length)
                
                # Check for standard multipart boundary
                content_type = self.headers.get('Content-Type', '')
                filename = "uploaded_experiment.csv"
                
                if 'multipart/form-data' in content_type:
                    # Parse filename and boundary
                    boundary = content_type.split("boundary=")[1].encode("utf-8")
                    parts = file_data.split(boundary)
                    # Extract raw CSV data
                    for part in parts:
                        if b"filename=" in part:
                            # Extract filename
                            fn_start = part.find(b"filename=\"") + 10
                            fn_end = part.find(b"\"", fn_start)
                            filename = part[fn_start:fn_end].decode("utf-8")
                            
                            # Extract body content
                            body_start = part.find(b"\r\n\r\n") + 4
                            body_end = part.rfind(b"\r\n")
                            file_data = part[body_start:body_end]
                            break

                save_path = os.path.join(os.getcwd(), "tools", filename)
                with open(save_path, "wb") as f:
                    f.write(file_data)
                
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(json.dumps({"success": True, "filename": filename, "path": f"tools/{filename}"}).encode("utf-8"))
            except Exception as e:
                self.send_response(500)
                self.end_headers()
                self.wfile.write(f"Upload failed: {e}".encode("utf-8"))
            return
            
        else:
            self.send_error(404, "Endpoint not found")

def run(server_class=HTTPServer, handler_class=DashboardHandler):
    server_address = ('', PORT)
    httpd = server_class(server_address, handler_class)
    print(f"==========================================================")
    print(f" LMP Telemetry Server running on port {PORT}")
    print(f" Open browser or visit: http://localhost:{PORT}")
    print(f"==========================================================")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping server...")
        httpd.server_close()

if __name__ == "__main__":
    run()
