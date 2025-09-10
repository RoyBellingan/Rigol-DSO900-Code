import pyvisa
import numpy as np
import csv

rm = pyvisa.ResourceManager("@py")
# Replace with your scope's IP
scope = rm.open_resource("TCPIP0::192.168.1.162::INSTR")

# Set the timeout to 5 seconds (5000 ms)
scope.timeout = 5000

print(scope.query("*IDN?"))  # identify scope

# Select channel and format
scope.write(":WAV:SOUR CHAN1")
scope.write(":WAV:FORM BYTE")

# Get waveform preamble for scaling
scope.write(":WAV:PRE?")
preamble = scope.query_ascii_values(":WAV:PRE?", container=np.ndarray)

# Preamble values explained
# preamble[0] = format
# preamble[1] = type
# preamble[2] = points
# preamble[3] = count
# preamble[4] = x_increment (XINC)
# preamble[5] = x_origin (XREF)
# preamble[6] = x_reference (XREF)
# preamble[7] = y_increment (YINC)
# preamble[8] = y_origin (YREF)
# preamble[9] = y_reference (YREF)

x_increment = preamble[4]
x_origin = preamble[5]
y_increment = preamble[7]
y_origin = preamble[8]
y_reference = preamble[9]

# Get the raw waveform data
data = scope.query_binary_values(":WAV:DATA?", datatype="B", container=np.ndarray)

# Scale the data to time and voltage
time = np.arange(len(data)) * x_increment + x_origin
voltage = (data - y_reference) * y_increment + y_origin

# Save to CSV file
with open("waveform_data.csv", "w", newline="") as csvfile:
    writer = csv.writer(csvfile)
    writer.writerow(["Time (s)", "Voltage (V)"])
    for t, v in zip(time, voltage):
        writer.writerow([t, v])

print("Waveform data saved to waveform_data.csv")

scope.close()
rm.close()
