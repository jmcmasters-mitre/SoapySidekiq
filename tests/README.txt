The provided test applications do the following:

cs15_validate: Puts the FPGA in ramp mode.  Sets Soapy to use CS16 samples.  
Does readStream on 10 blocks, testing the received rf_timestamps and validates 
the ramp on each block.  Then it does it again with sys_timestamps.

cs32_validate: Puts the FPGA in ramp mode.  Sets Soapy to use CF32 samples.  
Does readStream on 10 blocks.  Then it validates the samples to see if they 
are what the FPGA ramp sends.

rx_to_file: Receives a defined number of blocks then writes them to a file.

tx_from_file: Transmits the blocks in a file, repeating until Ctrl-c.

txtone: Generates the samples for a tone and transmits it until Ctrl-c.

txtone_1pps: Generates the samples for a tone and transmits it on 1pps.  
Continues until Ctrl-c.

test_api: The runs through all of the API commands without streaming to validate they give the right values and pass in invalid values and verify errors.

record_cs16: This receives samples and writes them to a file continuously until Ctrl-c.  This uses threading to have one receive thread and one write to file thread.  This is limited in sample rate due to speed of writing to file.

