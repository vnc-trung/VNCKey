# Hướng dẫn đăng VNC-Key lên GitHub

## Bước 1: Tạo tài khoản GitHub
1. Vào https://github.com → Sign up
2. Xác nhận email

## Bước 2: Tạo repository mới
1. Bấm nút **+** (góc phải trên) → **New repository**
2. Repository name: `VNCKey`
3. Description: `VNC-Key - Bộ gõ Tiếng Việt cho Windows`
4. Chọn **Public** (vì dựa trên mã nguồn mở GPL)
5. Tick **Add a README file**
6. License: chọn **GNU General Public License v3.0**
7. Bấm **Create repository**

## Bước 3: Upload source code (cách đơn giản nhất)
1. Vào repository vừa tạo
2. Bấm **Add file** → **Upload files**
3. Kéo thả toàn bộ thư mục Sources vào
4. Viết mô tả: "VNC-Key v1.0 - Initial release"
5. Bấm **Commit changes**

## Bước 4: Tạo Release (để người khác tải exe)
1. Ở trang repository, bấm **Releases** (bên phải)
2. Bấm **Create a new release**
3. Tag version: `v1.0.0`
4. Release title: `VNC-Key 1.0 - Phiên bản đầu tiên`
5. Mô tả:
   ```
   VNC-Key là bộ gõ tiếng Việt miễn phí cho Windows.
   - Hỗ trợ Terminal (Git Bash, Windows Terminal, mintty)
   - Thanh kéo tốc độ gõ Terminal
   - Dựa trên mã nguồn mở OpenKey
   ```
6. Kéo thả file **VNC-Key32.exe** và **VNC-Key64.exe** vào phần "Attach binaries"
7. Bấm **Publish release**

## Cách khác: Dùng Visual Studio (tích hợp sẵn)
1. Mở VNCKey.sln trong Visual Studio
2. Menu **Git** → **Create Git Repository...**
3. Chọn **GitHub** → đăng nhập
4. Repository name: `VNCKey`
5. Bấm **Create and Push**
→ Visual Studio sẽ tự tạo repo và upload code lên GitHub

## Link repository sau khi tạo
```
https://github.com/TEN-GITHUB-CUA-BAN/VNCKey
```
Thay `TEN-GITHUB-CUA-BAN` bằng username GitHub của bạn.
