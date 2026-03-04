return {
    display = {
        provider = 'ssd1306',
        opts = { addr = 0x3C, sda = 0, scl = 1, freq = 400000 }
    },
    sensor = {
        provider = 'sht40',
        opts = { addr = 0x44, sda = 0, scl = 1, freq = 100000 }
    }
}
