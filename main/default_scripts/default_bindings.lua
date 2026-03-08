-- ST7735 LCD (Air101 LCD module)
-- SPI mode: mosi=3, clk=2, cs=7, dc=6, res=10, bl=11
return {
    display = {
        provider = 'st7735',
        opts = { mosi = 3, clk = 2, cs = 7, dc = 6, res = 10, bl = 11, freq = 20000000 }
    },
    sensor = {
        provider = 'sht40',
        opts = { addr = 0x44, sda = 1, scl = 0, freq = 100000 }
    }
}
