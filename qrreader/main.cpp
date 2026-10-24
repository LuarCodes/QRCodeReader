// QR Reader - scan a QR code from the screen, an image file, or the clipboard -> text (auto-copied).
// Ctrl+Alt+Q anywhere = scan screen region (click without dragging = whole screen). Ctrl+V = paste image. Ctrl+O = open.
#include "../common/ui.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "../third/stb_image.h"
extern "C" {
#include "../third/quirc/quirc.h"
}
using namespace ui;

enum { ID_SCAN = 100, ID_PASTE, ID_OPEN, ID_COPY, ID_LINK, ID_TEXT };
static const int HOTKEY = 1;
static HWND gWnd, gText, gCopyBtn, gLinkBtn;
static Img gImg;
static std::vector<std::wstring> gCodes;
static std::wstring gInfo = L"Nothing scanned yet";
static std::wstring gStatus = L"Ready  \x00B7  Ctrl+Alt+Q scans the screen from anywhere";
static COLORREF gStatusColor = MUTED;

static void setStatus(const std::wstring& s, COLORREF c) { gStatus = s; gStatusColor = c; InvalidateRect(gWnd, nullptr, FALSE); }

static bool tryDecode(const std::vector<uint8_t>& gray, int w, int h, std::vector<std::wstring>& out) {
    quirc* q = quirc_new();
    if (!q || quirc_resize(q, w, h) < 0) { if (q) quirc_destroy(q); return false; }
    uint8_t* buf = quirc_begin(q, nullptr, nullptr);
    memcpy(buf, gray.data(), (size_t)w * h);
    quirc_end(q);
    int n = quirc_count(q);
    for (int i = 0; i < n; i++) {
        quirc_code code; quirc_data data;
        quirc_extract(q, i, &code);
        quirc_decode_error_t err = quirc_decode(&code, &data);
        if (err == QUIRC_ERROR_DATA_ECC) { quirc_flip(&code); err = quirc_decode(&code, &data); }
        if (!err) {
            std::wstring t = fromUtf8(std::string((const char*)data.payload, data.payload_len));
            if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
        }
    }
    quirc_destroy(q);
    return !out.empty();
}
static std::vector<std::wstring> decodeImg(const Img& im) {
    std::vector<std::wstring> out;
    int w = im.w, h = im.h;
    std::vector<uint8_t> g((size_t)w * h);
    for (size_t i = 0; i < g.size(); i++) {
        uint32_t p = im.px[i]; g[i] = (uint8_t)((((p >> 16) & 255) * 77 + ((p >> 8) & 255) * 150 + (p & 255) * 29) >> 8);
    }
    if (tryDecode(g, w, h, out)) return out;
    std::vector<uint8_t> inv(g.size());
    for (size_t i = 0; i < g.size(); i++) inv[i] = 255 - g[i];
    if (tryDecode(inv, w, h, out)) return out;
    if (w * h < 1200 * 1200) {  // small image: upscale 2x
        int W = w * 2, H = h * 2; std::vector<uint8_t> up((size_t)W * H);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) up[(size_t)y * W + x] = g[(size_t)(y / 2) * w + x / 2];
        if (tryDecode(up, W, H, out)) return out;
    }
    if (w > 2200 || h > 2200) {  // huge image: downscale 2x
        int W = w / 2, H = h / 2; std::vector<uint8_t> dn((size_t)W * H);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            size_t a = (size_t)(2 * y) * w + 2 * x;
            dn[(size_t)y * W + x] = (uint8_t)((g[a] + g[a + 1] + g[a + w] + g[a + w + 1]) / 4);
        }
        tryDecode(dn, W, H, out);
    }
    return out;
}
static bool isLink(const std::wstring& s) {
    auto st = [&](const wchar_t* p) { return _wcsnicmp(s.c_str(), p, wcslen(p)) == 0; };
    return st(L"http://") || st(L"https://") || st(L"www.");
}

static void process(Img im, const wchar_t* source) {
    double t = nowMs();
    gImg = std::move(im);
    gCodes = decodeImg(gImg);
    double ms = nowMs() - t;
    if (gCodes.empty()) {
        SetWindowTextW(gText, L"");
        EnableWindow(gCopyBtn, FALSE); EnableWindow(gLinkBtn, FALSE);
        gInfo = fmt(L"No QR code found in the %ls (%.1f ms)", source, ms);
        setStatus(L"No QR code found \x2014 try selecting a tighter area around it", WARN);
        return;
    }
    std::wstring all;
    for (size_t i = 0; i < gCodes.size(); i++) {
        if (gCodes.size() > 1) all += fmt(L"\x2014 QR %d \x2014\n", (int)i + 1);
        all += gCodes[i];
        if (i + 1 < gCodes.size()) all += L"\n\n";
    }
    SetWindowTextW(gText, toCrlf(all).c_str());
    EnableWindow(gCopyBtn, TRUE); EnableWindow(gLinkBtn, isLink(gCodes[0]));
    std::wstring copyText = gCodes.size() == 1 ? gCodes[0] : all;
    bool copied = setClipText(gWnd, copyText);
    gInfo = fmt(L"Found %d QR code%ls in the %ls  \x00B7  %.1f ms", (int)gCodes.size(), gCodes.size() > 1 ? L"s" : L"", source, ms);
    setStatus(copied ? L"\x2713 Decoded and copied to clipboard \x2014 paste it with Ctrl+V" : L"Decoded (clipboard busy, use Copy)", OK);
}

static bool loadImageFile(const std::wstring& path, Img& im) {
    std::string data;
    if (!readFile(path, data, 256ull << 20)) return false;
    int w, h, n;
    unsigned char* p = stbi_load_from_memory((const stbi_uc*)data.data(), (int)data.size(), &w, &h, &n, 4);
    if (!p) return false;
    im.w = w; im.h = h; im.px.resize((size_t)w * h);
    for (size_t i = 0; i < im.px.size(); i++) {
        unsigned char* q = p + i * 4;
        uint32_t a = q[3];  // composite on white so transparent QR PNGs work
        uint32_t r = (q[0] * a + 255 * (255 - a)) / 255, g = (q[1] * a + 255 * (255 - a)) / 255, b = (q[2] * a + 255 * (255 - a)) / 255;
        im.px[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    stbi_image_free(p);
    return true;
}
static void openPath(const std::wstring& path) {
    Img im;
    if (!loadImageFile(path, im)) { setStatus(L"Could not read that image (" + baseName(path) + L")", ERR); return; }
    process(std::move(im), L"image");
}
static bool clipPng(Img& im) {  // browsers & many apps also put a "PNG" format on the clipboard
    UINT fmt = RegisterClipboardFormatW(L"PNG");
    if (!IsClipboardFormatAvailable(fmt) || !openClip(gWnd)) return false;
    bool ok = false;
    if (HANDLE g = GetClipboardData(fmt)) {
        if (auto* p = (const stbi_uc*)GlobalLock(g)) {
            int w, h, n; unsigned char* d = stbi_load_from_memory(p, (int)GlobalSize(g), &w, &h, &n, 4);
            if (d) {
                im.w = w; im.h = h; im.px.resize((size_t)w * h);
                for (size_t i = 0; i < im.px.size(); i++) {
                    unsigned char* q = d + i * 4; uint32_t a = q[3];
                    uint32_t r = (q[0] * a + 255 * (255 - a)) / 255, gg = (q[1] * a + 255 * (255 - a)) / 255, b = (q[2] * a + 255 * (255 - a)) / 255;
                    im.px[i] = 0xFF000000u | (r << 16) | (gg << 8) | b;
                }
                stbi_image_free(d); ok = true;
            }
            GlobalUnlock(g);
        }
    }
    CloseClipboard(); return ok;
}
static void doPaste() {
    Img im; std::wstring file;
    if (getClipImage(gWnd, im, &file) || clipPng(im)) process(std::move(im), L"clipboard image");
    else if (!file.empty()) openPath(file);
    else setStatus(L"No image on the clipboard \x2014 copy a screenshot or image first", WARN);
}
static void doScan() {
    Shot s;
    if (!hideAndCapture(gWnd, s)) { bringToFront(gWnd); setStatus(L"Screen capture failed", ERR); return; }
    RECT r;
    bool ok = selectRegion(s, r, L"Drag around the QR code  \x00B7  click = whole screen  \x00B7  Esc", true);
    Img im; if (ok) im = crop(s, r);
    s.free();
    bringToFront(gWnd);
    if (!ok) { setStatus(L"Cancelled", MUTED); return; }
    process(std::move(im), L"screen");
}

static void paint(HDC dc, RECT) {
    header(dc, L"QR Reader", L"screen  \x00B7  image  \x00B7  clipboard   \x2192   text", 180);
    fillRound(dc, R(24, 70, 380, 434), S(20), CARD);
    RECT pv = R(44, 90, 340, 300);
    fillRound(dc, pv, S(14), EMPTY);
    if (gImg.w) { RECT in = pv; InflateRect(&in, -S(8), -S(8)); drawImgFit(dc, gImg, in); }
    else text(dc, L"Scan, paste or drop an image", pv, fBody, MUTED, CENTER1);

    fillRound(dc, R(420, 70, 416, 434), S(20), CARD);
    text(dc, L"Decoded text", R(440, 82, 380, 22), fBody, MUTED, LEFT1);
    fillRound(dc, R(440, 110, 376, 290), S(14), FIELD, BORDER);
    text(dc, gInfo, R(440, 462, 380, 22), fSmall, gCodes.empty() ? MUTED : OK, LEFT1);
    text(dc, gStatus, R(26, 512, 810, 22), fSmall, gStatusColor, LEFT1);
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        gWnd = h;
        button(h, ID_SCAN, L"Scan screen", R(44, 404, 340, 44), PRIMARY);
        button(h, ID_PASTE, L"Paste image", R(44, 456, 164, 36));
        button(h, ID_OPEN, L"Open image", R(220, 456, 164, 36));
        gText = edit(h, ID_TEXT, R(452, 120, 356, 272), ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL);
        gCopyBtn = button(h, ID_COPY, L"Copy text", R(440, 410, 184, 44), PRIMARY);
        gLinkBtn = button(h, ID_LINK, L"Open link", R(632, 410, 184, 44));
        EnableWindow(gCopyBtn, FALSE); EnableWindow(gLinkBtn, FALSE);
        DragAcceptFiles(h, TRUE);
        if (!RegisterHotKey(h, HOTKEY, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q'))
            setStatus(L"Ctrl+Alt+Q is used by another app \x2014 use the button instead", WARN);
        return 0;
    case WM_HOTKEY: if (w == HOTKEY) doScan(); return 0;
    case WM_DROPFILES: { auto f = dropped((HDROP)w); if (!f.empty()) openPath(f[0]); return 0; }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_SCAN: doScan(); break;
        case ID_PASTE: doPaste(); break;
        case ID_OPEN: {
            auto f = openFiles(h, L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tga;*.psd\0All files\0*.*\0", false);
            if (!f.empty()) openPath(f[0]);
            break;
        }
        case ID_COPY: {
            std::wstring t = getText(gText);
            if (setClipText(h, t)) { flash(gCopyBtn, L"\x2713  Copied!"); setStatus(L"\x2713 Copied to clipboard", OK); }
            break;
        }
        case ID_LINK:
            if (!gCodes.empty() && isLink(gCodes[0])) {
                std::wstring u = gCodes[0]; if (_wcsnicmp(u.c_str(), L"www.", 4) == 0) u = L"https://" + u;
                ShellExecuteW(h, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        }
        return 0;
    case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: return ctlColor(w);
    case WM_DRAWITEM: drawButton((DRAWITEMSTRUCT*)l); return TRUE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paintBuffered(h, paint); return 0;
    case WM_DESTROY: UnregisterHotKey(h, HOTKEY); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    init(inst);
    HWND h = createMain(L"QRReaderWnd", L"QR Reader", 860, 540, wndProc);
    ShowWindow(h, show); UpdateWindow(h);
    int argc; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) openPath(argv[1]);
    return loop([&](MSG& m) {
        if (ctrlKey(m, 'V') && GetForegroundWindow() == h) { doPaste(); return true; }
        if (ctrlKey(m, 'O')) { SendMessageW(h, WM_COMMAND, ID_OPEN, 0); return true; }
        return false;
    });
}
