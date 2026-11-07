// ============================================================================
//  TopClock —— 屏幕顶部居中的桌面时钟（C++ / Win32 / GDI+）
//  ---------------------------------------------------------------------------
//  特性：
//    * 无边框、置顶、悬浮于屏幕正上方居中，不占任务栏、不进 Alt+Tab
//    * 分层窗口（per-pixel alpha）实现圆角半透明毛玻璃面板
//    * 等宽数字排版，秒数跳动时不抖动
//    * 鼠标左键拖动位置，贴近顶部与中心自动吸附，位置记忆
//    * 右键菜单：显示秒 / 24 小时制 / 显示日期 / 字号 / 鼠标穿透 / 自动启动
//    * 托盘图标常驻，Ctrl+Alt+T 全局热键切换鼠标穿透
//
//  调试参数：
//    TopClock.exe --dump  <png路径>   离屏渲染面板预览图后退出
//
//  编译：见 build.bat（MSVC x64）
// ============================================================================

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

namespace gp = Gdiplus;

// ---------------------------------------------------------------- 常量定义 --
static const wchar_t* kClassName = L"TopClock.Window.Class";
static const wchar_t* kAppName = L"TopClock";
static const wchar_t* kMutexName = L"TopClock_SingleInstance_7B31F0";

static const UINT WM_TRAYICON = WM_APP + 1;
static const UINT_PTR kClockTimer = 1;
static const int kHotKeyId = 1;

enum MenuId
{
    IDM_SECONDS = 1001,
    IDM_HOUR24,
    IDM_SHOWDATE,
    IDM_SIZE_S,
    IDM_SIZE_M,
    IDM_SIZE_L,
    IDM_CLICKTHROUGH,
    IDM_CENTER,
    IDM_AUTOSTART,
    IDM_HIDE,
    IDM_ABOUT,
    IDM_EXIT
};

static const int kPosAuto = -999999;

// -------------------------------------------------------------------- 设置 --
struct Settings
{
    bool showSeconds = true;
    bool hour24 = true;
    bool showDate = true;
    bool clickThrough = false;
    int  scale = 100;      // 界面缩放百分比：85 / 100 / 120
    int  x = kPosAuto;     // kPosAuto 表示自动顶部居中
    int  y = kPosAuto;
};

static Settings g_settings;
static HWND     g_hwnd = nullptr;
static HICON    g_icon = nullptr;
static ULONG_PTR g_gdiToken = 0;
static bool     g_hover = false;
static bool     g_dragging = false;
static int      g_dragDX = 0, g_dragDY = 0;
static int      g_winW = 0, g_winH = 0;
static int      g_lastRenderW = 0;
static std::wstring g_lastTime, g_lastDate;
static bool     g_lastHover = false;
static bool     g_visible = true;

// ------------------------------------------------------------ 配置文件读写 --
static std::wstring ConfigPath()
{
    wchar_t dir[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH);
    std::wstring path = (n > 0 && n < MAX_PATH) ? std::wstring(dir) : std::wstring(L".");

    path += L"\\TopClock";
    CreateDirectoryW(path.c_str(), nullptr);
    path += L"\\settings.ini";
    return path;
}

static void LoadSettings(Settings& s)
{
    const std::wstring ini = ConfigPath();
    s.showSeconds  = GetPrivateProfileIntW(L"clock", L"showSeconds", 1, ini.c_str()) != 0;
    s.hour24       = GetPrivateProfileIntW(L"clock", L"hour24", 1, ini.c_str()) != 0;
    s.showDate     = GetPrivateProfileIntW(L"clock", L"showDate", 1, ini.c_str()) != 0;
    s.clickThrough = GetPrivateProfileIntW(L"clock", L"clickThrough", 0, ini.c_str()) != 0;
    s.scale        = GetPrivateProfileIntW(L"clock", L"scale", 100, ini.c_str());
    s.x            = GetPrivateProfileIntW(L"clock", L"x", kPosAuto, ini.c_str());
    s.y            = GetPrivateProfileIntW(L"clock", L"y", kPosAuto, ini.c_str());

    if (s.scale != 85 && s.scale != 100 && s.scale != 120) s.scale = 100;
}

static void SaveSettings(const Settings& s)
{
    const std::wstring ini = ConfigPath();
    auto put = [&](const wchar_t* key, int value) {
        wchar_t buf[32];
        swprintf_s(buf, L"%d", value);
        WritePrivateProfileStringW(L"clock", key, buf, ini.c_str());
    };
    put(L"showSeconds", s.showSeconds ? 1 : 0);
    put(L"hour24", s.hour24 ? 1 : 0);
    put(L"showDate", s.showDate ? 1 : 0);
    put(L"clickThrough", s.clickThrough ? 1 : 0);
    put(L"scale", s.scale);
    put(L"x", s.x);
    put(L"y", s.y);
}

// ------------------------------------------------------------------ 小工具 --
static UINT GetDpiSafe(HWND hwnd)
{
    typedef UINT(WINAPI* PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow fn = (PFN_GetDpiForWindow)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn) {
        UINT dpi = fn(hwnd);
        if (dpi) return dpi;
    }
    HDC dc = GetDC(nullptr);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return dpi > 0 ? (UINT)dpi : 96;
}

static void ClampToScreen(int& x, int& y, int w, int h)
{
    RECT vs = {};
    vs.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vs.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vs.right  = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vs.bottom = vs.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (w > vs.right - vs.left) w = vs.right - vs.left;
    if (h > vs.bottom - vs.top) h = vs.bottom - vs.top;

    x = std::max<LONG>(vs.left, std::min<LONG>(x, vs.right - w));
    y = std::max<LONG>(vs.top, std::min<LONG>(y, vs.bottom - h));
}

// ------------------------------------------------------------ 文字与排版 --
// GDI+ 默认的 MeasureString 会给整个字串叠加约 1/6 em 的左右留白，
// 直接拿它做等宽网格会把数字撑开。用 GenericTypographic 排版格式可以去掉
// 这层留白，字符步进再用 “10 个字 - 1 个字” 的差值精确标定。
static void InitTypoFormat(gp::StringFormat& fmt)
{
    fmt.SetFormatFlags(gp::StringFormatFlagsNoWrap | gp::StringFormatFlagsNoClip |
                       gp::StringFormatFlagsMeasureTrailingSpaces);
    fmt.SetAlignment(gp::StringAlignmentNear);
    fmt.SetLineAlignment(gp::StringAlignmentNear);
}

static float MeasureW(gp::Graphics& g, gp::Font& f, const wchar_t* text, int len = -1)
{
    gp::StringFormat fmt(gp::StringFormat::GenericTypographic());
    InitTypoFormat(fmt);
    gp::RectF box;
    g.MeasureString(text, len, &f, gp::RectF(0, 0, 2000, 2000), &fmt, &box);
    return box.Width;
}

static gp::Font* CreateFontPx(const wchar_t* family, float px)
{
    gp::FontFamily ff(family);
    if (!ff.IsAvailable()) {
        gp::FontFamily fallback(L"Segoe UI");
        if (fallback.IsAvailable())
            return new gp::Font(&fallback, px, gp::FontStyleRegular, gp::UnitPixel);
        gp::FontFamily generic(L"Microsoft YaHei UI");
        if (generic.IsAvailable())
            return new gp::Font(&generic, px, gp::FontStyleRegular, gp::UnitPixel);
        gp::FontFamily arial(L"Arial");
        return new gp::Font(&arial, px, gp::FontStyleRegular, gp::UnitPixel);
    }
    return new gp::Font(&ff, px, gp::FontStyleRegular, gp::UnitPixel);
}

// 逐字符等宽网格排版：数字占据固定宽度，秒数跳变时整体不抖动
static void DrawTabular(gp::Graphics& g, gp::Font& f, const std::wstring& text,
                        const gp::Color& color, float left, float top,
                        float cell, float colonAdv)
{
    gp::SolidBrush brush(color);
    gp::StringFormat fmt(gp::StringFormat::GenericTypographic());
    InitTypoFormat(fmt);

    float x = left;
    for (wchar_t ch : text) {
        wchar_t one[2] = { ch, 0 };
        float w = MeasureW(g, f, one, 1);
        float adv = (ch == L':') ? colonAdv : cell;
        g.DrawString(one, 1, &f, gp::PointF(x + (adv - w) * 0.5f, top), &fmt, &brush);
        x += adv;
    }
}

struct FontMetrics
{
    float digitAdv;   // 数字步进
    float colonAdv;   // 冒号步进
};

static FontMetrics MeasureFont(gp::Graphics& g, gp::Font& f)
{
    const float w1   = MeasureW(g, f, L"0", 1);
    const float w10  = MeasureW(g, f, L"0000000000", 10);
    const float w00  = MeasureW(g, f, L"00", 2);
    const float w0c0 = MeasureW(g, f, L"0:0", 3);

    FontMetrics m = {};
    m.digitAdv = (w10 - w1) / 9.0f;
    if (m.digitAdv <= 0) m.digitAdv = w1 > 0 ? w1 : f.GetSize();
    m.colonAdv = m.digitAdv + (w0c0 - w00);
    if (m.colonAdv <= 0) m.colonAdv = m.digitAdv * 0.5f;
    return m;
}

static void AddRoundRect(gp::GraphicsPath& path, const gp::RectF& r, float radius)
{
    float d = radius * 2.0f;
    if (d > r.Width)  d = r.Width;
    if (d > r.Height) d = r.Height;

    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

// ------------------------------------------------------------ 时间格式化 --
struct ClockStrings
{
    std::wstring time;
    std::wstring date;
};

static void BuildStrings(const Settings& s, const SYSTEMTIME& st, ClockStrings& out)
{
    wchar_t buf[64];

    int hour = st.wHour;
    if (!s.hour24) {
        hour = st.wHour % 12;
        if (hour == 0) hour = 12;
    }
    if (s.showSeconds)
        swprintf_s(buf, L"%02d:%02d:%02d", hour, st.wMinute, st.wSecond);
    else
        swprintf_s(buf, L"%02d:%02d", hour, st.wMinute);
    out.time = buf;

    static const wchar_t* week[] = { L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六" };
    swprintf_s(buf, L"%d月%d日 %s", st.wMonth, st.wDay, week[st.wDayOfWeek % 7]);
    out.date = buf;

    if (!s.hour24)
        out.date = (st.wHour < 12 ? L"上午 " : L"下午 ") + out.date;
}

// ------------------------------------------------------- 面板离屏绘制（核心）--
// 按给定缩放把时钟面板画进一张 32bpp ARGB 位图，返回位图与尺寸。
// 该函数不依赖窗口句柄，因此既用于屏幕显示，也用于 --dump 预览。
static std::unique_ptr<gp::Bitmap> BuildPanel(const ClockStrings& cs, float s,
                                              bool hover, int* outW, int* outH)
{
    // ---- 度量 ----
    gp::Bitmap probe(1, 1, PixelFormat32bppARGB);
    gp::Graphics gm(&probe);
    gm.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);

    const float timePx = 25.0f * s;
    const float datePx = 12.5f * s;
    std::unique_ptr<gp::Font> fTime(CreateFontPx(L"Segoe UI Semibold", timePx));
    std::unique_ptr<gp::Font> fDate(CreateFontPx(L"Segoe UI", datePx));

    const FontMetrics tm = MeasureFont(gm, *fTime);
    const float cell = tm.digitAdv;
    const float colonAdv = tm.colonAdv;

    int digits = 0, colons = 0;
    for (wchar_t c : cs.time) {
        if (c == L':') ++colons; else ++digits;
    }
    const float timeW = cell * digits + colonAdv * colons;
    const float timeH = fTime->GetHeight(&gm);
    const float dateW = g_settings.showDate ? MeasureW(gm, *fDate, cs.date.c_str()) : 0.0f;
    const float dateH = fDate->GetHeight(&gm);

    // ---- 版面常量（全部按 DPI/字号缩放）----
    const float padX = 17.0f * s;
    const float padY = 8.0f * s;
    const float gap = 12.0f * s;
    const float dotD = 3.2f * s;                      // 时间与日期之间的强调色圆点
    const float radius = 13.0f * s;
    const float sepW = g_settings.showDate ? (gap + dotD + gap) : 0.0f;

    const float contentH = std::max(timeH, dateH);
    int W = (int)std::ceil(padX * 2 + timeW + sepW + dateW);
    int H = (int)std::ceil(contentH + padY * 2);
    W = std::max(W, (int)(72 * s));
    H = std::max(H, (int)(30 * s));

    *outW = W;
    *outH = H;

    // ---- 绘制 ----
    std::unique_ptr<gp::Bitmap> bmp(new gp::Bitmap(W, H, PixelFormat32bppARGB));
    gp::Graphics g(bmp.get());
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
    g.SetCompositingQuality(gp::CompositingQualityHighQuality);
    g.Clear(gp::Color(0, 0, 0, 0));

    gp::RectF panel(0.0f, 0.0f, (gp::REAL)W, (gp::REAL)H);
    panel.Inflate(-0.5f, -0.5f);

    gp::GraphicsPath path;
    AddRoundRect(path, panel, radius);

    // 玻璃质感渐变底（半透明，让桌面内容隐约透出）
    gp::Color topCol = hover ? gp::Color(246, 52, 56, 68) : gp::Color(240, 42, 45, 54);
    gp::Color botCol = hover ? gp::Color(228, 32, 35, 44) : gp::Color(222, 24, 26, 33);
    gp::LinearGradientBrush backGrad(gp::PointF(0, 0), gp::PointF(0, (gp::REAL)H), topCol, botCol);
    g.FillPath(&backGrad, &path);

    // 1px 描边，营造边缘高光
    gp::Pen border(gp::Color(38, 255, 255, 255), std::max(1.0f, s));
    g.DrawPath(&border, &path);

    // 时间：按字形墨迹（而非行高）做视觉居中，-0.025em 为实测光学修正量
    const float timeTop = (H - timeH) * 0.5f - timePx * 0.025f;
    DrawTabular(g, *fTime, cs.time, gp::Color(255, 246, 247, 251), padX, timeTop, cell, colonAdv);

    // 分隔圆点 + 日期
    if (g_settings.showDate) {
        const float cx = padX + timeW + gap + dotD * 0.5f;
        const float cy = H * 0.5f;
        gp::SolidBrush dot(gp::Color(255, 96, 165, 250));
        g.FillEllipse(&dot, cx - dotD * 0.5f, cy - dotD * 0.5f, dotD, dotD);

        const float dateTop = (H - dateH) * 0.5f + datePx * 0.02f;
        gp::SolidBrush dateBrush(gp::Color(172, 178, 184, 198));
        gp::StringFormat fmt(gp::StringFormat::GenericTypographic());
        InitTypoFormat(fmt);
        g.DrawString(cs.date.c_str(), (INT)cs.date.size(), fDate.get(),
                     gp::PointF(cx + dotD * 0.5f + gap, dateTop), &fmt, &dateBrush);
    }

    return bmp;
}

// --------------------------------------------------------------- 屏幕呈现 --
static void Render(HWND hwnd, bool force)
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    ClockStrings cs;
    BuildStrings(g_settings, st, cs);

    if (!force && cs.time == g_lastTime && cs.date == g_lastDate && g_hover == g_lastHover)
        return;
    g_lastTime = cs.time;
    g_lastDate = cs.date;
    g_lastHover = g_hover;

    const float s = GetDpiSafe(hwnd) / 96.0f * (g_settings.scale / 100.0f);

    int W = 0, H = 0;
    std::unique_ptr<gp::Bitmap> bmpHold = BuildPanel(cs, s, g_hover, &W, &H);
    gp::Bitmap& bmp = *bmpHold;
    g_winW = W;
    g_winH = H;

    // ---- 位置 ----
    // 显示项开关会改变面板宽度：手动定位过的话，按中心点补偿，
    // 避免面板只朝一侧生长导致视觉偏移。
    if (g_settings.x != kPosAuto && g_lastRenderW > 0 && W != g_lastRenderW && !g_dragging) {
        g_settings.x += (g_lastRenderW - W) / 2;
        SaveSettings(g_settings);
    }
    g_lastRenderW = W;

    int x, y;
    if (g_settings.x == kPosAuto || g_settings.y == kPosAuto) {
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof(mi) };
        if (!GetMonitorInfoW(mon, &mi)) {
            mi.rcWork.left = 0;
            mi.rcWork.top = 0;
            mi.rcWork.right = GetSystemMetrics(SM_CXSCREEN);
            mi.rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
        }
        x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - W) / 2;
        y = mi.rcWork.top;
    } else {
        x = g_settings.x;
        y = g_settings.y;
        ClampToScreen(x, y, W, H);
    }

    // ---- 转成预乘 alpha 的 DIB，交给分层窗口 ----
    HDC screenDC = GetDC(nullptr);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -H;              // 自上而下
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dibBits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (!dib) {
        ReleaseDC(nullptr, screenDC);
        return;
    }

    gp::BitmapData bd = {};
    gp::Rect lockRect(0, 0, W, H);
    bmp.LockBits(&lockRect, gp::ImageLockModeRead, PixelFormat32bppARGB, &bd);
    {
        const BYTE* src = (const BYTE*)bd.Scan0;
        BYTE* dst = (BYTE*)dibBits;
        for (int row = 0; row < H; ++row) {
            const BYTE* sRow = src + (size_t)row * bd.Stride;
            BYTE* dRow = dst + (size_t)row * W * 4;
            for (int col = 0; col < W; ++col) {
                const BYTE a = sRow[col * 4 + 3];
                if (a == 0) {
                    dRow[col * 4 + 0] = 0;
                    dRow[col * 4 + 1] = 0;
                    dRow[col * 4 + 2] = 0;
                    dRow[col * 4 + 3] = 0;
                } else if (a == 255) {
                    dRow[col * 4 + 0] = sRow[col * 4 + 0];
                    dRow[col * 4 + 1] = sRow[col * 4 + 1];
                    dRow[col * 4 + 2] = sRow[col * 4 + 2];
                    dRow[col * 4 + 3] = 255;
                } else {
                    dRow[col * 4 + 0] = (BYTE)(sRow[col * 4 + 0] * a / 255);
                    dRow[col * 4 + 1] = (BYTE)(sRow[col * 4 + 1] * a / 255);
                    dRow[col * 4 + 2] = (BYTE)(sRow[col * 4 + 2] * a / 255);
                    dRow[col * 4 + 3] = a;
                }
            }
        }
    }
    bmp.UnlockBits(&bd);

    HDC memDC = CreateCompatibleDC(screenDC);
    HGDIOBJ oldObj = SelectObject(memDC, dib);

    POINT dstPt = { x, y };
    SIZE  size  = { W, H };
    POINT srcPt = { 0, 0 };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd, screenDC, &dstPt, &size, memDC, &srcPt, 0, &blend, ULW_ALPHA);

    SelectObject(memDC, oldObj);
    DeleteDC(memDC);
    DeleteObject(dib);
    ReleaseDC(nullptr, screenDC);
}

// --------------------------------------------------------------- 位置吸附 --
static void SnapPosition()
{
    if (g_settings.x == kPosAuto) return;

    const float s = GetDpiSafe(g_hwnd) / 96.0f * (g_settings.scale / 100.0f);
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return;

    if (std::abs(g_settings.y - mi.rcWork.top) <= (int)(14 * s))
        g_settings.y = mi.rcWork.top;

    const int monCenter = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left) / 2;
    if (std::abs((g_settings.x + g_winW / 2) - monCenter) <= (int)(30 * s))
        g_settings.x = monCenter - g_winW / 2;
}

static void CenterTop()
{
    g_settings.x = kPosAuto;
    g_settings.y = kPosAuto;
    SaveSettings(g_settings);
    Render(g_hwnd, true);
}

// ------------------------------------------------------------- 鼠标穿透 --
static void SetClickThrough(bool enable)
{
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    if (enable) ex |= WS_EX_TRANSPARENT;
    else        ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);

    g_settings.clickThrough = enable;
    SaveSettings(g_settings);

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcscpy_s(nid.szInfoTitle, enable ? L"鼠标穿透已开启" : L"鼠标穿透已关闭");
    wcscpy_s(nid.szInfo, enable
        ? L"时钟不再响应鼠标操作，按 Ctrl+Alt+T 可恢复。"
        : L"时钟已可拖动与右键操作。");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// --------------------------------------------------------------- 开机启动 --
static bool IsAutoStart()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;

    wchar_t buf[1024] = {};
    DWORD cb = sizeof(buf);
    bool exists = RegQueryValueExW(key, kAppName, nullptr, nullptr, (LPBYTE)buf, &cb) == ERROR_SUCCESS;
    RegCloseKey(key);
    return exists;
}

static void SetAutoStart(bool enable)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    if (enable) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring value = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(key, kAppName, 0, REG_SZ, (const BYTE*)value.c_str(),
                       (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kAppName);
    }
    RegCloseKey(key);
}

// --------------------------------------------------------------- 托盘图标 --
static HICON CreateTrayIcon(int sz)
{
    gp::Bitmap bmp(sz, sz, PixelFormat32bppARGB);
    {
        gp::Graphics g(&bmp);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        g.Clear(gp::Color(0, 0, 0, 0));

        const float m = sz * 0.05f;
        gp::GraphicsPath circle;
        circle.AddEllipse(m, m, sz - 2 * m, sz - 2 * m);
        gp::LinearGradientBrush grad(gp::PointF(0, 0), gp::PointF(0, (gp::REAL)sz),
                                     gp::Color(255, 96, 165, 250), gp::Color(255, 37, 99, 235));
        g.FillPath(&grad, &circle);

        gp::Pen pen(gp::Color(255, 255, 255, 255), sz * 0.085f);
        pen.SetStartCap(gp::LineCapRound);
        pen.SetEndCap(gp::LineCapRound);
        gp::PointF c(sz / 2.0f, sz / 2.0f);
        g.DrawLine(&pen, c, gp::PointF(c.X, sz * 0.22f));                        // 分针
        g.DrawLine(&pen, c, gp::PointF(sz * 0.73f, c.Y + sz * 0.08f));           // 时针
    }

    HBITMAP color = nullptr;
    bmp.GetHBITMAP(gp::Color(0, 0, 0, 0), &color);
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

static void AddTrayIcon(HWND hwnd)
{
    g_icon = CreateTrayIcon(32);

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g_icon;
    wcscpy_s(nid.szTip, L"TopClock —— 顶部时钟");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void RemoveTrayIcon(HWND hwnd)
{
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (g_icon) { DestroyIcon(g_icon); g_icon = nullptr; }
}

// ------------------------------------------------------------------- 菜单 --
static void ApplyCommand(UINT id)
{
    switch (id) {
    case IDM_SECONDS:
        g_settings.showSeconds = !g_settings.showSeconds;
        SaveSettings(g_settings);
        Render(g_hwnd, true);
        break;
    case IDM_HOUR24:
        g_settings.hour24 = !g_settings.hour24;
        SaveSettings(g_settings);
        Render(g_hwnd, true);
        break;
    case IDM_SHOWDATE:
        g_settings.showDate = !g_settings.showDate;
        SaveSettings(g_settings);
        Render(g_hwnd, true);
        break;
    case IDM_SIZE_S:
    case IDM_SIZE_M:
    case IDM_SIZE_L:
        g_settings.scale = (id == IDM_SIZE_S) ? 85 : (id == IDM_SIZE_M) ? 100 : 120;
        SaveSettings(g_settings);
        Render(g_hwnd, true);
        break;
    case IDM_CLICKTHROUGH:
        SetClickThrough(!g_settings.clickThrough);
        break;
    case IDM_CENTER:
        CenterTop();
        break;
    case IDM_AUTOSTART:
        SetAutoStart(!IsAutoStart());
        break;
    case IDM_HIDE:
        ShowWindow(g_hwnd, SW_HIDE);
        g_visible = false;
        break;
    case IDM_ABOUT:
        MessageBoxW(g_hwnd,
                    L"TopClock 1.0\n\n屏幕顶部居中显示的轻量时钟。\n\n"
                    L"· 左键拖动可移动位置，靠近顶部与中心会自动吸附\n"
                    L"· 右键打开设置菜单\n"
                    L"· Ctrl+Alt+T 切换鼠标穿透\n"
                    L"· 设置保存在 %APPDATA%\\TopClock\\settings.ini",
                    L"关于 TopClock", MB_OK | MB_ICONINFORMATION);
        break;
    case IDM_EXIT:
        DestroyWindow(g_hwnd);
        break;
    default:
        break;
    }
}

static void ShowMenu(POINT pt)
{
    HMENU menu = CreatePopupMenu();

    auto item = [&](UINT id, const wchar_t* text, bool checked, bool radio = false) {
        // 0x00000200 即 MF_RADIOCHECK（部分 SDK 头文件未导出该宏）
        const UINT MF_RADIO = 0x00000200U;
        UINT flags = MF_STRING;
        if (checked) flags |= radio ? (MF_CHECKED | MF_RADIO) : MF_CHECKED;
        AppendMenuW(menu, flags, id, text);
    };

    item(IDM_SECONDS, L"显示秒", g_settings.showSeconds);
    item(IDM_HOUR24, L"24 小时制", g_settings.hour24);
    item(IDM_SHOWDATE, L"显示日期", g_settings.showDate);

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    item(IDM_SIZE_S, L"字号：小", g_settings.scale == 85, true);
    item(IDM_SIZE_M, L"字号：中", g_settings.scale == 100, true);
    item(IDM_SIZE_L, L"字号：大", g_settings.scale == 120, true);

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    item(IDM_CLICKTHROUGH, L"鼠标穿透（Ctrl+Alt+T）", g_settings.clickThrough);
    item(IDM_AUTOSTART, L"开机自动启动", IsAutoStart());

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_CENTER, L"回到顶部居中");
    AppendMenuW(menu, MF_STRING, IDM_HIDE, L"隐藏时钟");
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"关于");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

    // 无激活窗口无法成为前台窗口，弹出菜单可能无法在点击空白处时关闭；
    // 这里临时去掉 WS_EX_NOACTIVATE，弹出结束后再恢复。
    LONG_PTR exStyle = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_NOACTIVATE);
    SetForegroundWindow(g_hwnd);

    UINT cmd = TrackPopupMenu(menu,
                              TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                              pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, exStyle);
    DestroyMenu(menu);

    if (cmd) ApplyCommand(cmd);
}

// --------------------------------------------------------------- 窗口过程 --
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        SetTimer(hwnd, kClockTimer, 100, nullptr);
        AddTrayIcon(hwnd);
        RegisterHotKey(hwnd, kHotKeyId, MOD_CONTROL | MOD_ALT, 'T');
        return 0;

    case WM_TIMER:
        if (wp == kClockTimer) Render(hwnd, false);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_LBUTTONDOWN: {
        POINT pt;
        GetCursorPos(&pt);
        RECT rc;
        GetWindowRect(hwnd, &rc);
        g_dragDX = pt.x - rc.left;
        g_dragDY = pt.y - rc.top;
        g_dragging = true;
        SetCapture(hwnd);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!g_hover) {
            g_hover = true;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            Render(hwnd, false);
        }
        if (g_dragging) {
            POINT pt;
            GetCursorPos(&pt);
            int nx = pt.x - g_dragDX;
            int ny = pt.y - g_dragDY;
            ClampToScreen(nx, ny, g_winW, g_winH);
            g_settings.x = nx;
            g_settings.y = ny;
            SetWindowPos(hwnd, HWND_TOPMOST, nx, ny, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_hover = false;
        Render(hwnd, false);
        return 0;

    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = false;
            ReleaseCapture();
            SnapPosition();
            SaveSettings(g_settings);
            Render(hwnd, true);
        }
        return 0;

    case WM_RBUTTONUP: {
        POINT pt;
        GetCursorPos(&pt);
        ShowMenu(pt);
        return 0;
    }

    case WM_HOTKEY:
        if (wp == kHotKeyId) SetClickThrough(!g_settings.clickThrough);
        return 0;

    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            ShowMenu(pt);
        } else if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
            if (!g_visible) {
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                g_visible = true;
                Render(hwnd, true);
            } else {
                ShowWindow(hwnd, SW_HIDE);
                g_visible = false;
            }
        }
        return 0;

    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        Render(hwnd, true);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kClockTimer);
        UnregisterHotKey(hwnd, kHotKeyId);
        RemoveTrayIcon(hwnd);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --------------------------------------------------------------- 预览导出 --
// 调试用：把面板合成到一块模拟深色桌面上并保存为 PNG，便于检查排版。
// scaleOverride 为 0 时使用当前设置的缩放。
static int DumpPreview(const wchar_t* outPath, float scaleOverride = 0.0f)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    ClockStrings cs;
    BuildStrings(g_settings, st, cs);

    const float s = scaleOverride > 0 ? scaleOverride : g_settings.scale / 100.0f;
    int W = 0, H = 0;
    std::unique_ptr<gp::Bitmap> panel = BuildPanel(cs, s, false, &W, &H);
    wprintf(L"panel: s=%.3f W=%d H=%d\n", s, W, H);

    const int CW = 900, CH = 200;
    gp::Bitmap canvas(CW, CH, PixelFormat32bppARGB);
    {
        gp::Graphics g(&canvas);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        gp::LinearGradientBrush bg(gp::PointF(0, 0), gp::PointF(0, (gp::REAL)CH),
                                   gp::Color(255, 30, 41, 59), gp::Color(255, 15, 23, 42));
        g.FillRectangle(&bg, 0, 0, CW, CH);
        g.DrawImage(panel.get(), (CW - W) / 2, 4, W, H);
    }

    CLSID pngClsid = {};
    UINT num = 0, size = 0;
    gp::GetImageEncodersSize(&num, &size);
    if (size == 0) return 1;

    std::unique_ptr<BYTE[]> buf(new BYTE[size]);
    auto info = (gp::ImageCodecInfo*)buf.get();
    gp::GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(info[i].MimeType, L"image/png") == 0) {
            pngClsid = info[i].Clsid;
            break;
        }
    }
    return canvas.Save(outPath, &pngClsid, nullptr) == gp::Ok ? 0 : 1;
}

// ------------------------------------------------------------------ 入口 --
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int)
{
    // 高 DPI：优先 Per-Monitor V2（manifest 已声明，此处兜底）
    typedef BOOL(WINAPI* PFN_SetCtx)(HANDLE);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto setCtx = (PFN_SetCtx)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setCtx) setCtx((HANDLE)-4);
    }

    gp::GdiplusStartupInput gdiInput;
    if (gp::GdiplusStartup(&g_gdiToken, &gdiInput, nullptr) != gp::Ok)
        return 1;

    LoadSettings(g_settings);

    // ---- 调试：离屏渲染一张预览图 ----
    if (lpCmdLine && wcsstr(lpCmdLine, L"--dump")) {
        std::wstring outPath = L"panel.png";
        float scaleArg = 0.0f;
        {
            int argc = 0;
            LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            if (argv && argc >= 3) outPath = argv[2];
            if (argv && argc >= 4) scaleArg = (float)_wtof(argv[3]) / 100.0f;
            if (argv) LocalFree(argv);
        }
        const int rc = DumpPreview(outPath.c_str(), scaleArg);
        gp::GdiplusShutdown(g_gdiToken);
        return rc;
    }

    // ---- 单实例 ----
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"TopClock 已经在运行了。", L"TopClock", MB_OK | MB_ICONINFORMATION);
        gp::GdiplusShutdown(g_gdiToken);
        return 0;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kClassName, kAppName, WS_POPUP,
        0, 0, 10, 10, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        gp::GdiplusShutdown(g_gdiToken);
        return 1;
    }

    if (g_settings.clickThrough)
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                          GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT);

    Render(hwnd, true);

    // 启动时归一化位置：DPI 或字号变化后，历史坐标可能让面板偏离中心，
    // 落在吸附范围内就自动拉回顶部居中。
    if (g_settings.x != kPosAuto) {
        const int oldX = g_settings.x;
        const int oldY = g_settings.y;
        SnapPosition();
        if (oldX != g_settings.x || oldY != g_settings.y) {
            SaveSettings(g_settings);
            Render(hwnd, true);
        }
    }

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    gp::GdiplusShutdown(g_gdiToken);
    if (mutex) CloseHandle(mutex);
    return 0;
}
