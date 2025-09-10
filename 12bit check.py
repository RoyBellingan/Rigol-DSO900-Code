# Add 12-bit precision verification
print(f"\n=== 12-bit Precision Verification ===")
print(f"Preamble format value: {preamble[0]}")
print(f"Expected for 12-bit: 1 (BYTE format) or 2 (WORD format)")

# Check if we should be using WORD format for 12-bit data
current_format = scope.query(":WAV:FORM?").strip()
print(f"Current waveform format: {current_format}")

# For 12-bit precision, you might need WORD format instead of BYTE
if current_format == "BYTE":
    print("WARNING: BYTE format only gives 8-bit precision (0-255)")
    print("For 12-bit precision, try WORD format")

    # Test with WORD format
    print("\nTesting WORD format for 12-bit precision...")
    try:
        scope.write(":WAV:FORM WORD")
        time.sleep(0.1)

        # Get new preamble with WORD format
        preamble_word = scope.query_ascii_values(":WAV:PRE?", container=np.ndarray)
        print(f"WORD format - Y increment: {preamble_word[7]:.9f}")
        print(f"WORD format - Y reference: {preamble_word[9]:.6f}")

        # Get a small sample to check data range
        scope.write(":WAV:POIN 100")  # Just get 100 points for testing
        data_word = scope.query_binary_values(
            ":WAV:DATA?", datatype="H", container=np.ndarray
        )  # 'H' for unsigned short

        print(f"WORD format data range: {data_word.min()} to {data_word.max()}")
        print(f"Expected 12-bit range: 0 to 4095")

        if data_word.max() > 255:
            print("✓ WORD format provides higher precision than BYTE")
            if data_word.max() <= 4095:
                print("✓ Data range consistent with 12-bit precision")
            else:
                print(f"? Data range ({data_word.max()}) exceeds 12-bit (4095)")

        # Check for 16-bit precision
        print(f"\n=== 16-bit Precision Check ===")
        if data_word.max() <= 65535:
            print("✓ Data range consistent with 16-bit precision (0-65535)")
            print("✓ Your scope provides 16-bit ADC resolution!")

            # Calculate actual bit depth from data range
            data_span = data_word.max() - data_word.min()
            effective_bits = np.log2(data_span)
            print(f"Data span: {data_span} levels")
            print(f"Effective resolution: ~{effective_bits:.1f} bits")

            # Check if it's signed 16-bit (centered around 32768)
            if abs(preamble_word[9] - 32768) < 1:
                print("✓ Y reference suggests 16-bit signed data format")
                print("  Data is likely offset by 32768 (midpoint)")

        # Reset to original settings for main data capture
        scope.write(":WAV:FORM BYTE")
        scope.write(":WAV:POIN 10000")  # Reset to full capture

    except Exception as e:
        print(f"Error testing WORD format: {e}")
