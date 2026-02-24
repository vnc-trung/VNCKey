# VNC-Key - Bộ gõ Tiếng Việt

Phiên bản 1.0 dành cho Windows

VNC-Key là phần mềm miễn phí được cải tiến và điều chỉnh dựa theo mã nguồn mở OpenKey.

## Tác giả
**Nguyễn Phước Trung, Vietnamchess**
Email: vnc@vietnamchess.vn

## Tính năng cải tiến

### Hỗ trợ Terminal (Git Bash, Windows Terminal, mintty)
- Gõ tiếng Việt hoạt động trong các terminal emulator
- Background thread xử lý output tránh hook timeout
- Key queuing chống lọt phím khi đang xử lý
- Thanh kéo "Tốc độ gõ Terminal" trong tab Hệ thống (5 mức)

### Chống race condition
- Delay giữa các backspace tránh mất ký tự
- Phát hiện tự động loại terminal (mintty, Windows Terminal, Win32 Console)

## Dựa trên
OpenKey - The Cross platform Open source Vietnamese Keyboard application
Copyright (C) 2019 Mai Vu Tuyen
License: GPL (mã nguồn mở)
