import os
import glob
import time
import pandas as pd
import streamlit as st
import plotly.express as px
import plotly.graph_objects as go
from datetime import datetime

# Set up Page Configuration
st.set_page_config(
    page_title="LMP System Telemetry",
    page_icon="📡",  # Keep browser tab icon but remove in-page emojis
    layout="wide",
    initial_sidebar_state="expanded"
)

# Professional Lab-Instrument CSS Styling (Zero AI Slop)
st.markdown("""
<style>
    /* Dark grid background similar to digital oscilloscopes */
    .stApp {
        background-color: #0c0e12;
        color: #d1d5db;
        font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
    }
    
    /* Muted sidebar configuration */
    section[data-testid="stSidebar"] {
        background-color: #11141a !important;
        border-right: 1px solid #1e2530 !important;
    }
    
    /* Clean custom card layout */
    .metric-box {
        background-color: #11151c;
        border: 1px solid #222a36;
        border-radius: 4px;
        padding: 16px 20px;
        min-height: 105px;
        display: flex;
        flex-direction: column;
        justify-content: space-between;
    }
    
    .metric-label {
        font-size: 0.7rem;
        font-weight: 600;
        color: #64748b;
        text-transform: uppercase;
        letter-spacing: 0.08em;
    }
    
    .metric-value {
        font-family: "Courier New", Courier, monospace;
        font-size: 1.8rem;
        font-weight: 600;
        color: #f3f4f6;
        margin-top: 6px;
    }
    
    .metric-delta {
        font-size: 0.75rem;
        font-weight: 500;
        margin-top: 4px;
    }
    
    .delta-normal {
        color: #64748b;
    }
    .delta-positive {
        color: #10b981;
    }
    .delta-negative {
        color: #ef4444;
    }
    
    /* Header layout */
    .dashboard-header {
        border-bottom: 1px solid #1e2530;
        padding-bottom: 20px;
        margin-bottom: 25px;
    }
    
    .dashboard-title {
        font-size: 1.5rem;
        font-weight: 700;
        color: #f3f4f6;
        letter-spacing: -0.02em;
    }
    
    .dashboard-subtitle {
        font-size: 0.85rem;
        color: #64748b;
        margin-top: 4px;
    }
    
    /* Navigation tabs styling */
    .stTabs [data-baseweb="tab-list"] {
        gap: 8px;
        border-bottom: 1px solid #1e2530;
    }
    
    .stTabs [data-baseweb="tab"] {
        background-color: transparent !important;
        border: none !important;
        color: #64748b !important;
        font-size: 0.85rem !important;
        font-weight: 500 !important;
        padding: 8px 16px !important;
    }
    
    .stTabs [aria-selected="true"] {
        color: #38bdf8 !important;
        border-bottom: 2px solid #38bdf8 !important;
    }
</style>
""", unsafe_allow_html=True)

# Helper function to generate custom HTML metric cards
def render_kpi_card(label, value, delta=None, delta_color="normal"):
    delta_html = ""
    if delta:
        class_name = "delta-positive" if delta_color == "positive" else "delta-negative" if delta_color == "negative" else "delta-normal"
        delta_html = f'<div class="metric-delta {class_name}">{delta}</div>'
    
    return f"""
    <div class="metric-box">
        <div class="metric-label">{label}</div>
        <div class="metric-value">{value}</div>
        {delta_html}
    </div>
    """

# Helper function to scan directory recursively for CSV files
def get_available_csvs():
    root_csvs = glob.glob("*.csv")
    tools_csvs = glob.glob("tools/*.csv")
    all_csvs = list(set(root_csvs + tools_csvs))
    return sorted(all_csvs)

# Helper function to load data safely
@st.cache_data(ttl=2)
def load_csv_data(filepath):
    try:
        df = pd.read_csv(filepath)
        required_cols = ["Timestamp", "SystemTimeMs", "RSSI_dBm", "SNR_dB", "PayloadSizeBytes", "Status"]
        for col in required_cols:
            if col not in df.columns:
                for c in df.columns:
                    if c.lower() == col.lower():
                        df.rename(columns={c: col}, inplace=True)
        return df
    except Exception as e:
        st.error(f"Error reading {filepath}: {e}")
        return pd.DataFrame()

# Clean Header (Lab system layout)
st.markdown("""
<div class="dashboard-header">
    <div class="dashboard-title">LMP | LORA MULTI-PACKET PROTOCOL EVALUATION</div>
    <div class="dashboard-subtitle">Laboratory system telemetry, physical link diagnostics, and fragmentation efficiency stats.</div>
</div>
""", unsafe_allow_html=True)

# Sidebar Configuration
st.sidebar.markdown("### DATA SOURCE")

available_files = get_available_csvs()
selected_file = None

if available_files:
    selected_file = st.sidebar.selectbox(
        "Select local log file:",
        options=available_files,
        index=0
    )
else:
    st.sidebar.info("No local CSV files found.")

uploaded_file = st.sidebar.file_uploader(
    "Or upload experiment CSV:",
    type=["csv"]
)

st.sidebar.markdown("---")
st.sidebar.markdown("### LIVE MONITORING")
auto_refresh = st.sidebar.checkbox("Auto-Refresh Telemetry", value=False)
refresh_interval = st.sidebar.slider("Reload interval (seconds)", min_value=2, max_value=30, value=5)

if auto_refresh:
    st.sidebar.caption(f"Auto-refresh active (every {refresh_interval}s)")

# Load the actual dataframe
df = pd.DataFrame()
source_name = ""

if uploaded_file is not None:
    df = pd.read_csv(uploaded_file)
    source_name = uploaded_file.name
elif selected_file:
    df = load_csv_data(selected_file)
    source_name = selected_file

if df.empty:
    st.warning("Please select or upload a valid non-empty telemetry CSV dataset to begin.")
    st.stop()

# Basic Data Cleaning
df["Timestamp"] = pd.to_datetime(df["Timestamp"], errors="coerce")
df["RSSI_dBm"] = pd.to_numeric(df["RSSI_dBm"], errors="coerce")
df["SNR_dB"] = pd.to_numeric(df["SNR_dB"], errors="coerce")
df["PayloadSizeBytes"] = pd.to_numeric(df["PayloadSizeBytes"], errors="coerce")

# Calculate Statistics
total_rows = len(df)
success_df = df[df["Status"] == "SUCCESS"]
failed_df = df[df["Status"] == "FAILED"]

total_rx_msg = len(success_df)
total_failed_msg = len(failed_df)
total_packets = total_rx_msg + total_failed_msg

pdr = (total_rx_msg / total_packets * 100.0) if total_packets > 0 else 0.0
avg_rssi = success_df["RSSI_dBm"].mean() if not success_df.empty else 0.0
avg_snr = success_df["SNR_dB"].mean() if not success_df.empty else 0.0
total_bytes = success_df["PayloadSizeBytes"].sum() if not success_df.empty else 0

# --- KPI DISPLAY SECTION ---
kpi_cols = st.columns(5)

with kpi_cols[0]:
    st.markdown(render_kpi_card("Total Msg Received", f"{total_packets}"), unsafe_allow_html=True)

with kpi_cols[1]:
    pdr_color = "positive" if pdr >= 95 else "normal" if pdr >= 85 else "negative"
    st.markdown(render_kpi_card("Reassembled (OK)", f"{total_rx_msg}", delta=f"{pdr:.2f}% PDR", delta_color=pdr_color), unsafe_allow_html=True)

with kpi_cols[2]:
    st.markdown(render_kpi_card("Avg RSSI", f"{avg_rssi:.1f} dBm" if total_rx_msg > 0 else "N/A"), unsafe_allow_html=True)

with kpi_cols[3]:
    st.markdown(render_kpi_card("Avg SNR", f"{avg_snr:.1f} dB" if total_rx_msg > 0 else "N/A"), unsafe_allow_html=True)

with kpi_cols[4]:
    st.markdown(render_kpi_card("Total Payload Rx", f"{total_bytes:,} B"), unsafe_allow_html=True)

st.markdown("<br>", unsafe_allow_html=True)

# --- ANALYTICS TABS ---
tab1, tab2, tab3 = st.tabs(["Link Diagnostics", "Throughput & Reliability", "Data Explorer"])

# Shared dark chart layout settings
plotly_layout_defaults = dict(
    template="plotly_dark",
    paper_bgcolor="rgba(17,21,28,1)",
    plot_bgcolor="rgba(17,21,28,1)",
    font=dict(family="Courier New, monospace", size=11, color="#94a3b8"),
    xaxis=dict(gridcolor="#1e2530", linecolor="#222a36", zeroline=False),
    yaxis=dict(gridcolor="#1e2530", linecolor="#222a36", zeroline=False),
    margin=dict(l=50, r=30, t=50, b=50)
)

with tab1:
    if total_rx_msg > 0:
        success_df_sorted = success_df.sort_values(by="SystemTimeMs")
        x_axis = "Timestamp" if success_df_sorted["Timestamp"].notna().any() else success_df_sorted.index
        
        # Sync Timeline Chart
        fig_line = go.Figure()
        fig_line.add_trace(go.Scatter(
            x=success_df_sorted[x_axis],
            y=success_df_sorted["RSSI_dBm"],
            name="RSSI (dBm)",
            line=dict(color="#38bdf8", width=1.5),
            yaxis="y1"
        ))
        fig_line.add_trace(go.Scatter(
            x=success_df_sorted[x_axis],
            y=success_df_sorted["SNR_dB"],
            name="SNR (dB)",
            line=dict(color="#818cf8", width=1.5),
            yaxis="y2"
        ))
        
        fig_line.update_layout(
            title=dict(text="RSSI & SNR Timeline", font=dict(family="sans-serif", size=14, color="#f3f4f6")),
            xaxis=dict(title=dict(text="Sequence Timeline")),
            yaxis=dict(title=dict(text="RSSI (dBm)", font=dict(color="#38bdf8")), tickfont=dict(color="#38bdf8")),
            yaxis2=dict(title=dict(text="SNR (dB)", font=dict(color="#818cf8")), tickfont=dict(color="#818cf8"), anchor="x", overlaying="y", side="right"),
            legend=dict(x=0.01, y=0.99, bgcolor="rgba(17,21,28,0.8)", bordercolor="#222a36", borderwidth=1),
            **{k: v for k, v in plotly_layout_defaults.items() if k not in ["xaxis", "yaxis"]}
        )
        # Apply specific grid options for secondary axes
        fig_line.update_layout(
            xaxis=dict(gridcolor="#1e2530", linecolor="#222a36", zeroline=False),
            yaxis=dict(gridcolor="#1e2530", linecolor="#222a36", zeroline=False),
            yaxis2=dict(gridcolor="#1e2530", zeroline=False)
        )
        st.plotly_chart(fig_line, use_container_width=True)
        
        # Histograms
        hist_col1, hist_col2 = st.columns(2)
        with hist_col1:
            fig_rssi = px.histogram(
                success_df, x="RSSI_dBm", 
                title="RSSI Distribution Profile",
                color_discrete_sequence=["#38bdf8"],
                labels={"RSSI_dBm": "RSSI (dBm)"}
            )
            fig_rssi.update_layout(bargap=0.05, **plotly_layout_defaults)
            fig_rssi.update_layout(title=dict(text="RSSI Distribution Profile", font=dict(family="sans-serif", size=13, color="#f3f4f6")))
            st.plotly_chart(fig_rssi, use_container_width=True)
            
        with hist_col2:
            fig_snr = px.histogram(
                success_df, x="SNR_dB", 
                title="SNR Distribution Profile",
                color_discrete_sequence=["#818cf8"],
                labels={"SNR_dB": "SNR (dB)"}
            )
            fig_snr.update_layout(bargap=0.05, **plotly_layout_defaults)
            fig_snr.update_layout(title=dict(text="SNR Distribution Profile", font=dict(family="sans-serif", size=13, color="#f3f4f6")))
            st.plotly_chart(fig_snr, use_container_width=True)
            
        # Scatter Plot RSSI vs SNR
        fig_scatter = px.scatter(
            success_df, x="RSSI_dBm", y="SNR_dB", 
            title="Signal Correlation Matrix (RSSI vs SNR)",
            color_discrete_sequence=["#38bdf8"],
            labels={"RSSI_dBm": "RSSI (dBm)", "SNR_dB": "SNR (dB)"}
        )
        fig_scatter.update_layout(**plotly_layout_defaults)
        fig_scatter.update_layout(
            title=dict(text="Signal Correlation Matrix (RSSI vs SNR)", font=dict(family="sans-serif", size=13, color="#f3f4f6")),
            margin=dict(l=50, r=30, t=50, b=50)
        )
        st.plotly_chart(fig_scatter, use_container_width=True)
        
    else:
        st.info("No diagnostic data available.")

with tab2:
    col_t1, col_t2 = st.columns(2)
    
    with col_t1:
        # Donut Chart
        labels = ['Success (OK)', 'Failed (CRC/Timeout)']
        values = [total_rx_msg, total_failed_msg]
        colors = ['#10b981', '#ef4444']
        
        fig_pdr = go.Figure(data=[go.Pie(
            labels=labels, 
            values=values, 
            hole=.45, 
            marker=dict(colors=colors),
            textinfo="percent+label",
            insidetextorientation="radial"
        )])
        fig_pdr.update_layout(
            title=dict(text="Packet Delivery Success Ratio (PDR)", font=dict(family="sans-serif", size=13, color="#f3f4f6")),
            template="plotly_dark",
            paper_bgcolor="rgba(17,21,28,1)",
            plot_bgcolor="rgba(17,21,28,1)",
            font=dict(family="Courier New, monospace", size=11, color="#94a3b8"),
            legend=dict(x=0.01, y=0.99, bgcolor="rgba(17,21,28,0.8)", bordercolor="#222a36", borderwidth=1)
        )
        st.plotly_chart(fig_pdr, use_container_width=True)
        
    with col_t2:
        # Cumulative Data Payload Bytes over Time
        if total_rx_msg > 0:
            success_df_sorted = success_df.sort_values(by="SystemTimeMs")
            success_df_sorted["CumulativeBytes"] = success_df_sorted["PayloadSizeBytes"].cumsum()
            x_axis_cum = "Timestamp" if success_df_sorted["Timestamp"].notna().any() else success_df_sorted.index
            
            fig_cum = px.line(
                success_df_sorted, x=x_axis_cum, y="CumulativeBytes",
                title="Cumulative Telemetry Data Volume Received (Bytes)",
                color_discrete_sequence=["#10b981"],
                labels={"CumulativeBytes": "Total Payload Bytes Received"}
            )
            fig_cum.update_layout(**plotly_layout_defaults)
            fig_cum.update_layout(title=dict(text="Cumulative Telemetry Data Volume Received (Bytes)", font=dict(family="sans-serif", size=13, color="#f3f4f6")))
            st.plotly_chart(fig_cum, use_container_width=True)
        else:
            st.info("No successful packet transmissions.")

with tab3:
    st.subheader("Interactive Data Log Explorer")
    
    # Minimal filter layout
    filter_cols = st.columns(3)
    with filter_cols[0]:
        status_filter = st.multiselect(
            "Status:",
            options=df["Status"].unique(),
            default=df["Status"].unique()
        )
    with filter_cols[1]:
        if total_rx_msg > 0:
            min_rssi = float(df["RSSI_dBm"].min())
            max_rssi = float(df["RSSI_dBm"].max())
            if min_rssi == max_rssi:
                min_rssi -= 5.0
                max_rssi += 5.0
            rssi_range = st.slider(
                "RSSI Range (dBm):",
                min_value=min_rssi,
                max_value=max_rssi,
                value=(min_rssi, max_rssi)
            )
        else:
            rssi_range = (0.0, 0.0)
    with filter_cols[2]:
        if total_rx_msg > 0:
            min_size = int(df["PayloadSizeBytes"].min())
            max_size = int(df["PayloadSizeBytes"].max())
            if min_size == max_size:
                min_size = 0
                max_size += 100
            size_range = st.slider(
                "Payload Size (Bytes):",
                min_value=min_size,
                max_value=max_size,
                value=(min_size, max_size)
            )
        else:
            size_range = (0, 0)
            
    # Apply filters
    filtered_df = df[df["Status"].isin(status_filter)]
    if total_rx_msg > 0:
        rssi_mask = (filtered_df["RSSI_dBm"].isna()) | ((filtered_df["RSSI_dBm"] >= rssi_range[0]) & (filtered_df["RSSI_dBm"] <= rssi_range[1]))
        size_mask = (filtered_df["PayloadSizeBytes"].isna()) | ((filtered_df["PayloadSizeBytes"] >= size_range[0]) & (filtered_df["PayloadSizeBytes"] <= size_range[1]))
        filtered_df = filtered_df[rssi_mask & size_mask]
        
    st.dataframe(filtered_df, use_container_width=True)
    
    # Download filtered CSV
    csv_data = filtered_df.to_csv(index=False).encode('utf-8')
    st.download_button(
        label="Download Filtered CSV Dataset",
        data=csv_data,
        file_name=f"filtered_{source_name}",
        mime="text/csv"
    )
    
    st.markdown("---")
    st.markdown("#### Log Information & Scope")
    meta_cols = st.columns(3)
    meta_cols[0].write(f"**Source log file:** `{source_name}`")
    meta_cols[1].write(f"**Total Records:** `{total_rows}` rows")
    if not df.empty and "Timestamp" in df.columns and df["Timestamp"].notna().any():
        duration = df["Timestamp"].max() - df["Timestamp"].min()
        meta_cols[2].write(f"**Duration:** `{duration}`")
    else:
        meta_cols[2].write("**Duration:** `N/A`")

# Live Auto-Refresh using st.rerun() to avoid full page blinks/reloads
if auto_refresh:
    time.sleep(refresh_interval)
    st.rerun()
