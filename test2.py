import pyvisa
import numpy as np
import csv
import time

rm = pyvisa.ResourceManager("@py")
# Replace with your scope's IP
scope = rm.open_resource("TCPIP0::192.168.1.162::INSTR")

# Set the timeout to 30 seconds (30000 ms) for waveform data transfer
scope.timeout = 30000

print(scope.query("*IDN?"))  # identify scope

# Check current scope settings
print("\nCurrent scope settings:")
try:
    print(f"Memory depth: {scope.query(':ACQ:MDEP?').strip()}")
except:
    pass

try:
    print(f"Waveform mode: {scope.query(':WAV:MODE?').strip()}")
except:
    pass

try:
    print(f"Waveform points: {scope.query(':WAV:POIN?').strip()}")
except:
    pass

try:
    print(f"Timebase: {scope.query(':TIM:SCAL?').strip()} s/div")
except:
    pass

try:
    print(f"Channel 1 scale: {scope.query(':CHAN1:SCAL?').strip()} V/div")
except:
    pass

try:
    print(f"Channel 1 offset: {scope.query(':CHAN1:OFFS?').strip()} V")
except:
    pass

# Select channel and format (using current settings)
scope.write(":WAV:SOUR CHAN1")
scope.write(":WAV:FORM BYTE")

# Small delay to ensure scope is ready
time.sleep(0.1)

# Get waveform preamble for scaling
preamble = scope.query_ascii_values(":WAV:PRE?", container=np.ndarray)

print("Raw preamble values:")
for i, val in enumerate(preamble):
    print(f"preamble[{i}] = {val}")

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

print(f"\nCorrected Waveform info:")
print(f"Data points: {int(preamble[2])}")
print(f"Sample rate: {1/x_increment:.1f} Hz")
print(f"Time range: {x_increment * preamble[2]:.6f} seconds")
print(f"Y increment: {y_increment:.6f} V")
print(f"Y origin (center voltage): {y_origin:.6f} V")
print(f"Y reference (scaling ref): {y_reference:.6f}")

# The Y origin from preamble might be different from scope's vertical position
# Let's check the actual scope vertical position
try:
    chan_offset = float(scope.query(":CHAN1:OFFS?").strip())
    chan_scale = float(scope.query(":CHAN1:SCAL?").strip())
    print(f"Channel offset: {chan_offset:.6f} V")
    print(f"Channel scale: {chan_scale:.6f} V/div")
    print(f"Expected center voltage: {-chan_offset:.6f} V")
except Exception as e:
    print(f"Could not read channel settings: {e}")

# Get the raw waveform data using current scope settings
try:
    print("\nDownloading waveform data...")
    data = scope.query_binary_values(":WAV:DATA?", datatype="B", container=np.ndarray)
    print(f"Successfully received {len(data)} data points")

    # Scale the data to time and voltage
    time_array = np.arange(len(data)) * x_increment + x_origin
    voltage = (data - y_reference) * y_increment + y_origin

    print("First 10 voltage values:")
    for i in range(min(10, len(voltage))):
        print(f"Point {i}: {voltage[i]:.6f} V")

    # Save to CSV file
    print("Saving data to CSV file...")
    with open("waveform_data.csv", "w", newline="") as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(["Time (s)", "Voltage (V)"])
        for t, v in zip(time_array, voltage):
            writer.writerow([t, v])

    print("Waveform data saved to waveform_data.csv")

except Exception as e:
    print(f"Error reading waveform data: {e}")
    print("Try increasing the timeout or checking scope settings")

scope.close()
rm.close()
