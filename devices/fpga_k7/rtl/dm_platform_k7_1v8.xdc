## dm_platform_k7_1v8.xdc — XC7K480T-FFG901 virtual-chip platform, 1.8 V I/O to the PXIe-6571.
## Every PACKAGE_PIN marked TODO must be set to your board's schematic.
## The 200 MHz clock / reset pins are taken from E:/fpga_demos/03-Vide/project_1/constrs/fft_top.xdc.

## ---- clock & reset
create_clock -period 5.000 -name sys_clk [get_ports sys_clk]
set_property PACKAGE_PIN AK34 [get_ports sys_clk]
set_property IOSTANDARD LVCMOS18 [get_ports sys_clk]
set_property PACKAGE_PIN AP34 [get_ports sys_rst_n]
set_property IOSTANDARD LVCMOS18 [get_ports sys_rst_n]
set_property PULLUP true [get_ports sys_rst_n]
set_false_path -from [get_ports sys_rst_n]

## ---- host USB-UART (921600 8N1)
set_property PACKAGE_PIN TODO [get_ports host_uart_rx]
set_property PACKAGE_PIN TODO [get_ports host_uart_tx]
set_property IOSTANDARD LVCMOS18 [get_ports {host_uart_rx host_uart_tx}]
set_false_path -from [get_ports host_uart_rx]
set_false_path -to   [get_ports host_uart_tx]

## ---- DUT pins to the 6571 (LVCMOS18; same order as the station pin map)
set_property PACKAGE_PIN TODO [get_ports swclk]
set_property PACKAGE_PIN TODO [get_ports swdio]
set_property PACKAGE_PIN TODO [get_ports nrst]
set_property PACKAGE_PIN TODO [get_ports {pa[0]}]
set_property PACKAGE_PIN TODO [get_ports {pa[1]}]
set_property PACKAGE_PIN TODO [get_ports {pa[2]}]
set_property PACKAGE_PIN TODO [get_ports {pa[3]}]
set_property PACKAGE_PIN TODO [get_ports spi_sck]
set_property PACKAGE_PIN TODO [get_ports spi_mosi]
set_property PACKAGE_PIN TODO [get_ports spi_miso]
set_property PACKAGE_PIN TODO [get_ports spi_cs_n]
set_property IOSTANDARD LVCMOS18 [get_ports {swclk swdio nrst pa[*] spi_sck spi_mosi spi_miso spi_cs_n}]
## Same pulls as the real chip, so open/short and leakage on the 6571 look alike
set_property PULLUP   true [get_ports swdio]
set_property PULLUP   true [get_ports nrst]
set_property PULLUP   true [get_ports spi_cs_n]
set_property PULLDOWN true [get_ports swclk]

## SWCLK from the 6571 clocks the chip's SW-DP: up to 50 MHz
create_clock -period 20.000 -name swclk [get_ports swclk]
set_clock_groups -asynchronous -group [get_clocks sys_clk] -group [get_clocks swclk]

## ---- LEDs (optional)
set_property PACKAGE_PIN TODO [get_ports {led[0]}]
set_property PACKAGE_PIN TODO [get_ports {led[1]}]
set_property PACKAGE_PIN TODO [get_ports {led[2]}]
set_property PACKAGE_PIN TODO [get_ports {led[3]}]
set_property IOSTANDARD LVCMOS18 [get_ports {led[*]}]

## ---- bitstream
## TODO: must match the board's bank 0 VCCO (CFGBVS VCCO + CONFIG_VOLTAGE 3.3 / 2.5 for a 3.3 / 2.5 V bank 0)
set_property CFGBVS GND [current_design]
set_property CONFIG_VOLTAGE 1.8 [current_design]
set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]
