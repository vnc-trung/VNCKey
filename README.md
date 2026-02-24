# VNC-Key
### Bộ gõ Tiếng Việt cho Windows và macOS

Phiên bản 1.0 – Ngày cập nhật: 02-02-2026

VNC-Key là phần mềm gõ tiếng Việt miễn phí, được cải tiến và điều chỉnh dựa theo mã nguồn mở [OpenKey](https://github.com/tuyenvm/OpenKey) của Mai Vũ Tuyên. Sử dụng kỹ thuật `Backspace`, loại bỏ lỗi gạch chân khó chịu ở bộ gõ mặc định. Hiển thị tiếng Việt tốt trong môi trường Terminal, Git Bash, ChessBase...

### Mã nguồn của ứng dụng được mở công khai, minh bạch dưới giấy phép GPL. Điều này nghĩa là bạn hoàn toàn có thể tải mã nguồn về tự build, cải tiến theo mục đích của bạn. Nếu bạn tái phân phối bản cải tiến của bạn, thì nó cũng phải là mã nguồn mở và ghi rõ tên bản gốc.

### Lưu ý, khi sử dụng VNC-Key, bạn nên tắt hẳn bộ gõ khác vì 2 chương trình bộ gõ sẽ xung đột nhau, dẫn đến thao tác không chính xác.

## Hỗ trợ kiểu gõ
- Telex
- VNI
- Simple Telex

## Bảng mã thông dụng
- Unicode (Unicode dựng sẵn).
- TCVN3 (ABC).
- VNI Windows.
- Unicode Compound (Unicode tổ hợp).
- Vietnamese Locale CP 1258.

## Tính năng

- **Hỗ trợ Terminal** — Gõ tiếng Việt trong Git Bash, Windows Terminal, mintty, ChessBase. Có thanh kéo tốc độ 5 mức, điều chỉnh nếu bị lỗi ký tự.
- **Đặt dấu kiểu mới** (Bật/Tắt) — Đặt dấu oà, uý thay vì òa, úy.
- **Quick Telex** (Bật/Tắt) — Gõ nhanh (cc=ch, gg=gi, kk=kh, nn=ng, qq=qu, pp=ph, tt=th).
- **Kiểm tra chính tả** (Bật/Tắt) — Hạn chế gõ sai từ tiếng Việt.
- **Phục hồi phím với từ sai** (Bật/Tắt) — Trả lại phím gốc nếu từ không hợp lệ.
- **Khởi động cùng Windows/macOS** (Bật/Tắt).
- **Biểu tượng hiện đại** (Bật/Tắt) — Biểu tượng phù hợp với chế độ Dark mode.
- **Đổi chế độ gõ bằng phím tắt** — Tùy chọn phím tắt chuyển Việt/Anh.
- **Sửa lỗi trên trình duyệt Chromium** (Bật/Tắt) — Chrome, Edge, Brave...
- **Sửa lỗi gạch chân trên macOS** (Bật/Tắt).
- **Tạm tắt VNC-Key bằng phím Ctrl/Cmd/Alt** (Bật/Tắt).
- **Cho phép dùng f z w j làm phụ âm đầu** (Bật/Tắt).
- **Gõ tắt phụ âm đầu:** f→ph, j→gi, w→qu (Bật/Tắt).
- **Gõ tắt phụ âm cuối:** g→ng, h→nh, k→ch (Bật/Tắt).
- **Macro** — Tính năng gõ tắt không giới hạn ký tự, tự động viết hoa.
- **Chuyển chế độ thông minh** (Bật/Tắt) — Bạn đang dùng chế độ gõ Tiếng Việt trên ứng dụng A, bạn chuyển qua ứng dụng B trước đó bạn dùng chế độ gõ Tiếng Anh, VNC-Key sẽ tự động chuyển qua chế độ gõ Tiếng Anh cho bạn, khi bạn quay lại ứng dụng A, VNC-Key sẽ chuyển lại chế độ gõ Tiếng Việt.
- **Viết hoa chữ cái đầu câu** (Bật/Tắt) — Tự ghi hoa chữ cái đầu câu khi kết thúc câu hoặc xuống hàng.
- **Tự ghi nhớ bảng mã theo ứng dụng** (Bật/Tắt) — Phù hợp cho các bạn dùng Photoshop, CAD, ChessBase... với các bảng mã VNI, TCVN3. VNC-Key tự ghi nhớ ứng dụng nào dùng bảng mã nào.
- **Công cụ chuyển mã** — Hỗ trợ chuyển mã qua lại văn bản, thích hợp cho việc chuyển đổi văn bản cũ viết bằng VNI, TCVN3 qua Unicode. Hỗ trợ phím tắt chuyển mã nhanh.

## Cài đặt

### Windows

**Cách 1 — Dùng file build sẵn:**
1. Tải file `VNC-Key32.exe` hoặc `VNC-Key64.exe` từ trang [Releases](../../releases).
2. Chạy trực tiếp, không cần cài đặt.

**Cách 2 — Tự build từ mã nguồn:**
1. Tải mã nguồn từ trang [Releases](../../releases) hoặc clone repository.
2. Mở `Sources/VNCKey/win32/VNCKey/VNCKey.sln` bằng Visual Studio 2019 trở lên.
3. Chọn cấu hình **Release** — **x86** (32-bit) hoặc **x64** (64-bit).
4. Nhấn **Build** → file exe nằm trong thư mục `Release` hoặc `x64/Release`.

### macOS
1. Mở `Sources/VNCKey/macOS/VNCKey.xcodeproj` bằng Xcode.
2. Nhấn **Build and Run**.
3. Vào **System Preferences → Security & Privacy → Accessibility**, cấp quyền cho VNC-Key. **Không tắt quyền này khi đang dùng VNC-Key.**

## Lưu ý
- Nên tắt hẳn bộ gõ khác (UniKey, GoTiengViet...) khi dùng VNC-Key để tránh xung đột.
- Nếu gõ tiếng Việt bị lỗi trong Terminal, vào tab **Hệ thống** và kéo thanh tốc độ về phía **Chậm**.
- VNC-Key hỗ trợ chạy với quyền Admin nếu cần gõ tiếng Việt trong các ứng dụng chạy quyền cao.

## Tác giả
- **Nguyễn Phước Trung, Vietnamchess**
- Email: vnc@vietnamchess.vn

## Bản gốc
VNC-Key được phát triển dựa trên mã nguồn mở [OpenKey](https://github.com/tuyenvm/OpenKey) của **Mai Vũ Tuyên**, phát hành theo giấy phép **GPL**. Xin cảm ơn tác giả gốc đã chia sẻ mã nguồn cho cộng đồng.
