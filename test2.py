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

# Add this line to get all the captured data:
scope.write(":WAV:MODE MAX")  # Change from NORMAL to MAXIMUM mode

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

print(f"\nWaveform scaling info:")
print(f"Data points: {int(preamble[2])}")
print(f"Sample rate: {1/x_increment:.1f} Hz")
print(f"Time range: {x_increment * preamble[2]:.6f} seconds")
print(f"Y increment: {y_increment:.6f} V")
print(f"Y origin (from preamble): {y_origin:.6f} V")
print(f"Y reference (scaling ref): {y_reference:.6f}")


# Configure for 16-bit precision capture
print("\n=== Configuring for 16-bit precision capture ===")
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

print(f"16-bit scaling parameters:")
print(f"X increment: {x_increment_16:.9f} s")
print(f"Y increment: {y_increment_16:.9f} V")
print(f"Y reference: {y_reference_16:.1f}")
print(f"Y origin: {y_origin_16:.6f} V")

# Get the actual scope vertical position settings
scope_offset = 0.0
scope_scale = 1.0
try:
    scope_offset = float(scope.query(":CHAN1:OFFS?").strip())
    scope_scale = float(scope.query(":CHAN1:SCAL?").strip())
    print(f"Scope offset (vertical position): {scope_offset:.6f} V")
    print(f"Scope scale: {scope_scale:.6f} V/div")
    print(f"Screen center represents: {-scope_offset:.6f} V")
except Exception as e:
    print(f"Could not read channel settings: {e}")

# Get the raw waveform data using 16-bit precision
try:
    print("\nDownloading 16-bit waveform data...")
    data = scope.query_binary_values(
        ":WAV:DATA?", datatype="H", container=np.ndarray
    )  # "H" for unsigned short (16-bit)
    print(f"Successfully received {len(data)} data points")
    print(f"16-bit data range: {data.min()} to {data.max()}")

    # Scale the data to time and voltage using 16-bit parameters
    time_array = np.arange(len(data)) * x_increment_16 + x_origin_16

    # Calculate voltage using 16-bit preamble values
    voltage_preamble_16 = (data - y_reference_16) * y_increment_16 + y_origin_16

    # Use corrected scaling with scope offset
    voltage_corrected_16 = (data - y_reference_16) * y_increment_16 - scope_offset

    print("16-bit scaling comparison:")
    print("First 5 points:")
    for i in range(min(5, len(data))):
        print(
            f"Point {i}: Raw={data[i]}, Preamble={voltage_preamble_16[i]:.8f}V, Corrected={voltage_corrected_16[i]:.8f}V"
        )

    # Use the corrected scaling based on scope settings
    voltage = voltage_corrected_16

    # Save to CSV file
    print("\nSaving data to CSV file using corrected scaling...")
    with open("waveform_data.csv", "w", newline="") as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(["Time (s)", "Voltage (V)"])
        for t, v in zip(time_array, voltage):
            writer.writerow([t, v])

    print("Waveform data saved to waveform_data.csv")
    print(f"Data range: {voltage.min():.6f}V to {voltage.max():.6f}V")

except Exception as e:
    print(f"Error reading waveform data: {e}")
    print("Try increasing the timeout or checking scope settings")

# Optional: grab all screenshots
png = scope.query_binary_values(":DISP:DATA? PNG", datatype="B", container=bytes)
with open("screenshot.png", "wb") as f:
    f.write(png)

scope.close()
rm.close()
