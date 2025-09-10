import pyvisa
import numpy as np
import csv
import time
import os
from datetime import datetime
from io import StringIO
import sys

# Create a folder with current date and time
now = datetime.now()
folder_name = now.strftime("%Y%m%d_%H%M%S")
os.makedirs(folder_name, exist_ok=True)

# Create a custom print function that also writes to file
log_file_path = os.path.join(folder_name, "run_log.txt")
log_file = open(log_file_path, "w")


def log_print(*args, **kwargs):
    # Print to console
    print(*args, **kwargs)
    # Also write to file
    print(*args, **kwargs, file=log_file)
    log_file.flush()  # Ensure immediate writing


# Replace all print statements with log_print
rm = pyvisa.ResourceManager("@py")
# Replace with your scope's IP
scope = rm.open_resource("TCPIP0::192.168.1.162::INSTR")

# Set the timeout to 30 seconds (30000 ms) for waveform data transfer
scope.timeout = 30000

log_print(f"=== Oscilloscope Data Acquisition Run ===")
log_print(f"Start time: {now.strftime('%Y-%m-%d %H:%M:%S')}")
log_print(f"Data folder: {folder_name}")
log_print("")

log_print(scope.query("*IDN?"))  # identify scope

# Check current scope settings
log_print("\nCurrent scope settings:")
try:
    log_print(f"Memory depth: {scope.query(':ACQ:MDEP?').strip()}")
except:
    pass

try:
    log_print(f"Waveform mode: {scope.query(':WAV:MODE?').strip()}")
except:
    pass

try:
    log_print(f"Waveform points: {scope.query(':WAV:POIN?').strip()}")
except:
    pass

try:
    log_print(f"Timebase: {scope.query(':TIM:SCAL?').strip()} s/div")
except:
    pass

try:
    log_print(f"Channel 1 scale: {scope.query(':CHAN1:SCAL?').strip()} V/div")
except:
    pass

try:
    log_print(f"Channel 1 offset: {scope.query(':CHAN1:OFFS?').strip()} V")
except:
    pass

# Select channel and format (using current settings)
scope.write(":WAV:SOUR CHAN1")
scope.write(":WAV:FORM BYTE")

# Add this line to get all the captured data:
scope.write(":WAV:MODE MAX")  # Change from NORMAL to MAXIMUM mode

# Small delay to ensure scope is ready
time.sleep(0.1)

# Get waveform preamble for scaling
preamble = scope.query_ascii_values(":WAV:PRE?", container=np.ndarray)

log_print("Raw preamble values:")
# for i, val in enumerate(preamble):
#    log_print(f"preamble[{i}] = {val}")

# Correct preamble interpretation for RIGOL DHO900 series:
# preamble[0] = format
# preamble[1] = type
# preamble[2] = points
# preamble[3] = count
# preamble[4] = x_increment (XINC)
# preamble[5] = x_origin (XREF)
# preamble[6] = x_reference (XREF)
# preamble[7] = y_increment (YINC)
# preamble[8] = y_origin (YORG)
# preamble[9] = y_reference (YREF)

x_increment = preamble[4]
x_origin = preamble[5]
y_increment = preamble[7]
y_origin = preamble[8]  # This should be the voltage at the center of the screen
y_reference = preamble[9]  # This is the reference value for scaling

log_print(f"\nWaveform scaling info:")
log_print(f"Data points: {int(preamble[2])}")
log_print(f"Sample rate: {1/x_increment:.1f} Hz")
log_print(f"Time range: {x_increment * preamble[2]:.6f} seconds")
log_print(f"Y increment: {y_increment:.6f} V")
log_print(f"Y origin (from preamble): {y_origin:.6f} V")
log_print(f"Y reference (scaling ref): {y_reference:.6f}")


# Configure for 16-bit precision capture
log_print("\n=== Configuring for 16-bit precision capture ===")
scope.write(":WAV:FORM WORD")  # Use WORD format for 16-bit precision
scope.write(":WAV:POIN 10000")  # Set full capture points

# Get updated preamble for 16-bit data
preamble_16bit = scope.query_ascii_values(":WAV:PRE?", container=np.ndarray)

# Extract 16-bit scaling parameters
x_increment_16 = preamble_16bit[4]
x_origin_16 = preamble_16bit[5]
y_increment_16 = preamble_16bit[7]
y_origin_16 = preamble_16bit[8]
y_reference_16 = preamble_16bit[9]

log_print(f"16-bit scaling parameters:")
log_print(f"X increment: {x_increment_16:.9f} s")
log_print(f"Y increment: {y_increment_16:.9f} V")
log_print(f"Y reference: {y_reference_16:.1f}")
log_print(f"Y origin: {y_origin_16:.6f} V")

# Get the actual scope vertical position settings
scope_offset = 0.0
scope_scale = 1.0
try:
    scope_offset = float(scope.query(":CHAN1:OFFS?").strip())
    scope_scale = float(scope.query(":CHAN1:SCAL?").strip())
    log_print(f"Scope offset (vertical position): {scope_offset:.6f} V")
    log_print(f"Scope scale: {scope_scale:.6f} V/div")
    log_print(f"Screen center represents: {-scope_offset:.6f} V")
except Exception as e:
    log_print(f"Could not read channel settings: {e}")

# Get the raw waveform data using 16-bit precision
try:
    log_print("\nDownloading 16-bit waveform data...")
    data = scope.query_binary_values(
        ":WAV:DATA?", datatype="H", container=np.ndarray
    )  # "H" for unsigned short (16-bit)
    log_print(f"Successfully received {len(data)} data points")
    log_print(f"16-bit data range: {data.min()} to {data.max()}")

    # Scale the data to time and voltage using 16-bit parameters
    time_array = np.arange(len(data)) * x_increment_16 + x_origin_16

    # Calculate voltage using 16-bit preamble values
    voltage_preamble_16 = (data - y_reference_16) * y_increment_16 + y_origin_16

    # Use corrected scaling with scope offset
    voltage_corrected_16 = (data - y_reference_16) * y_increment_16 - scope_offset

    log_print("16-bit scaling comparison:")
    log_print("First 5 points:")
    for i in range(min(5, len(data))):
        log_print(
            f"Point {i}: Raw={data[i]}, Preamble={voltage_preamble_16[i]:.8f}V, Corrected={voltage_corrected_16[i]:.8f}V"
        )

    # Use the corrected scaling based on scope settings
    voltage = voltage_corrected_16

    # Save to CSV file in the run folder
    csv_file_path = os.path.join(folder_name, "waveform_data.csv")
    log_print(f"\nSaving data to CSV file using corrected scaling...")
    with open(csv_file_path, "w", newline="") as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(["Time (s)", "Voltage (V)"])
        for t, v in zip(time_array, voltage):
            writer.writerow([t, v])

    log_print(f"Waveform data saved to {csv_file_path}")
    log_print(f"Data range: {voltage.min():.6f}V to {voltage.max():.6f}V")

except Exception as e:
    log_print(f"Error reading waveform data: {e}")
    log_print("Try increasing the timeout or checking scope settings")

# Save screenshot in the run folder
try:
    log_print("\nCapturing screenshot...")
    png = scope.query_binary_values(":DISP:DATA? PNG", datatype="B", container=bytes)
    screenshot_path = os.path.join(folder_name, "screenshot.png")
    with open(screenshot_path, "wb") as f:
        f.write(png)
    log_print(f"Screenshot saved to {screenshot_path}")
except Exception as e:
    log_print(f"Error capturing screenshot: {e}")

# Close connections
scope.close()
rm.close()

# Close log file
end_time = datetime.now()
log_print(f"\n=== Run completed ===")
log_print(f"End time: {end_time.strftime('%Y-%m-%d %H:%M:%S')}")
log_print(f"Total duration: {(end_time - now).total_seconds():.2f} seconds")
log_print(f"All files saved in folder: {folder_name}")

log_file.close()

print(f"\n=== Run Summary ===")
print(f"All data saved in folder: {folder_name}")
print(f"Files created:")
print(f"  - log.txt (complete log of this session)")
print(f"  - waveform_data.csv (measurement data)")
print(f"  - screenshot.png (scope display)")
