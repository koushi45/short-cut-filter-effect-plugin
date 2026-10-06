#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aviutl2_sdk/plugin2.h"

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kPluginName[] = L"short-cut-filter-effect-plugin";
constexpr wchar_t kWindowName[] = L"short-cut-filter-effect-plugin";
constexpr wchar_t kWindowClass[] = L"ShortCutFilterEffectPluginWindow";
constexpr UINT kButtonBase = 1000;
constexpr UINT kHotKeyBase = 2000;
constexpr int kSlotCount = 10;
constexpr int kButtonWidth = 86;
constexpr int kButtonHeight = 68;
constexpr int kButtonGap = 8;
constexpr int kMargin = 8;
constexpr char kProjectKey[] = "slots.v1";

struct Slot {
    std::wstring effect_name;
    std::string effect_name_utf8;
    std::string effect_block;
};

struct CapturedItem {
    std::wstring name;
    std::string value;
};

HINSTANCE g_instance{};
EDIT_HANDLE* g_edit_handle{};
HWND g_main{};
std::array<Slot, kSlotCount> g_slots{};
std::array<std::wstring, kSlotCount> g_menu_names{};
int g_scroll{};
int g_wheel_delta{};
bool g_dark_mode{};
HBRUSH g_dark_background_brush{};
HBRUSH g_dark_button_brush{};
constexpr COLORREF kDarkBackground = RGB(32, 32, 32);
constexpr COLORREF kDarkButton = RGB(45, 45, 48);
constexpr COLORREF kDarkButtonHot = RGB(62, 62, 66);
constexpr COLORREF kDarkBorder = RGB(83, 83, 83);
constexpr COLORREF kDarkText = RGB(240, 240, 240);

HMENU control_id(UINT id) {
    return reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id));
}

bool system_uses_dark_mode() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

void apply_theme(HWND hwnd) {
    if (!hwnd) return;
    BOOL dark = g_dark_mode;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    SetWindowTheme(hwnd, g_dark_mode ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        SetWindowTheme(child, g_dark_mode ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        InvalidateRect(child, nullptr, TRUE);
    }
    InvalidateRect(hwnd, nullptr, TRUE);
}

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::string trim_copy(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string(text);
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        size_t line_end = end;
        if (line_end > start && text[line_end - 1] == '\r') --line_end;
        lines.push_back(text.substr(start, line_end - start));
        if (end == text.size()) break;
        start = end + 1;
    }
    return lines;
}

bool is_section_header(std::string_view line) {
    const auto trimmed = trim_copy(line);
    return trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']';
}

std::optional<std::pair<std::string, std::string>> parse_key_value(std::string_view line) {
    const size_t pos = line.find('=');
    if (pos == std::string_view::npos) return std::nullopt;
    return std::pair{trim_copy(line.substr(0, pos)), trim_copy(line.substr(pos + 1))};
}

bool value_matches_effect_name(std::string_view value, std::string_view effect_name_utf8) {
    std::string trimmed = trim_copy(value);
    if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"') {
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    }
    return trimmed == effect_name_utf8;
}

std::optional<std::string> capture_effect_block(std::string_view alias, std::string_view effect_name_utf8) {
    const auto lines = split_lines(alias);
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto key_value = parse_key_value(lines[i]);
        if (!key_value) continue;
        const std::string& key = key_value->first;
        if (key != "effect.name" && key != "name" && !ends_with(key, ".effect.name")) continue;
        if (!value_matches_effect_name(key_value->second, effect_name_utf8)) continue;

        size_t begin = i;
        while (begin > 0 && !is_section_header(lines[begin - 1])) --begin;
        if (begin > 0) --begin;

        size_t end = i + 1;
        while (end < lines.size() && !is_section_header(lines[end])) ++end;

        std::string block;
        for (size_t line = begin; line < end; ++line) {
            block.append(lines[line]);
            block.push_back('\n');
        }
        return block;
    }
    return std::nullopt;
}

std::vector<CapturedItem> captured_items(const Slot& slot) {
    std::vector<CapturedItem> items;
    for (const auto line : split_lines(slot.effect_block)) {
        const auto key_value = parse_key_value(line);
        if (!key_value) continue;
        const std::string& key = key_value->first;
        if (key == "effect.name" || key == "name" || ends_with(key, ".effect.name")) continue;
        if (key.empty() || key.front() == '[') continue;
        items.push_back({utf8_to_wide(key), key_value->second});
    }
    return items;
}

std::string hex_encode(std::string_view input) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 2);
    for (unsigned char ch : input) {
        out.push_back(digits[ch >> 4]);
        out.push_back(digits[ch & 0x0F]);
    }
    return out;
}

std::string hex_decode(std::string_view input) {
    auto value_of = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
    };
    std::string out;
    if (input.size() % 2 != 0) return out;
    out.reserve(input.size() / 2);
    for (size_t i = 0; i < input.size(); i += 2) {
        const int hi = value_of(input[i]);
        const int lo = value_of(input[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

fs::path slots_path() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(g_instance, path, MAX_PATH);
    return fs::path(path).replace_extension(L".slots");
}

std::string read_binary(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool write_binary(const fs::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return output.good();
}

std::string serialize_slots() {
    std::string text = "SCFEP1\n";
    for (int i = 0; i < kSlotCount; ++i) {
        text += std::to_string(i + 1);
        text.push_back('\t');
        text += hex_encode(g_slots[i].effect_name_utf8);
        text.push_back('\t');
        text += hex_encode(g_slots[i].effect_block);
        text.push_back('\n');
    }
    return text;
}

void deserialize_slots(std::string_view text) {
    for (auto& slot : g_slots) slot = {};
    const auto lines = split_lines(text);
    if (lines.empty() || lines[0] != "SCFEP1") return;
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string line(lines[i]);
        if (line.empty()) continue;
        const size_t first = line.find('\t');
        const size_t second = first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
        if (first == std::string::npos || second == std::string::npos) continue;
        int slot_number = 0;
        auto result = std::from_chars(line.data(), line.data() + first, slot_number);
        if (result.ec != std::errc{} || slot_number < 1 || slot_number > kSlotCount) continue;
        auto& slot = g_slots[slot_number - 1];
        slot.effect_name_utf8 = hex_decode(std::string_view(line).substr(first + 1, second - first - 1));
        slot.effect_name = utf8_to_wide(slot.effect_name_utf8);
        slot.effect_block = hex_decode(std::string_view(line).substr(second + 1));
    }
}

void load_slots_file() {
    const auto path = slots_path();
    if (!fs::exists(path)) return;
    deserialize_slots(read_binary(path));
}

void save_slots_file() {
    write_binary(slots_path(), serialize_slots());
}

void refresh_buttons() {
    if (!g_main) return;
    for (int i = 0; i < kSlotCount; ++i) {
        HWND button = GetDlgItem(g_main, kButtonBase + i);
        if (!button) continue;
        std::wstring text = std::to_wstring(i + 1);
        if (!g_slots[i].effect_name.empty()) {
            text += L"\r\n";
            text += g_slots[i].effect_name;
        }
        SetWindowTextW(button, text.c_str());
    }
}

void update_scroll() {
    if (!g_main) return;
    RECT client{};
    GetClientRect(g_main, &client);
    const int content_width = kMargin * 2 + kSlotCount * kButtonWidth + (kSlotCount - 1) * kButtonGap;
    const int page = std::max(1, static_cast<int>(client.right - client.left));
    const int max_scroll = std::max(0, content_width - page);
    g_scroll = std::clamp(g_scroll, 0, max_scroll);

    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMin = 0;
    info.nMax = std::max(0, content_width - 1);
    info.nPage = static_cast<UINT>(page);
    info.nPos = g_scroll;
    SetScrollInfo(g_main, SB_HORZ, &info, TRUE);
    ShowScrollBar(g_main, SB_HORZ, max_scroll > 0);

    for (int i = 0; i < kSlotCount; ++i) {
        HWND button = GetDlgItem(g_main, kButtonBase + i);
        if (!button) continue;
        const int x = kMargin + i * (kButtonWidth + kButtonGap) - g_scroll;
        MoveWindow(button, x, kMargin, kButtonWidth, kButtonHeight, TRUE);
    }
}

void set_scroll(int position) {
    RECT client{};
    GetClientRect(g_main, &client);
    const int content_width = kMargin * 2 + kSlotCount * kButtonWidth + (kSlotCount - 1) * kButtonGap;
    g_scroll = std::clamp(position, 0,
                          std::max(0, content_width - static_cast<int>(client.right - client.left)));
    update_scroll();
}

OBJECT_HANDLE selected_or_focus_object(EDIT_SECTION* edit) {
    if (OBJECT_HANDLE focused = edit->get_focus_object()) return focused;
    return edit->get_selected_object_num() > 0 ? edit->get_selected_object(0) : nullptr;
}

int next_object_section_index(std::string_view alias) {
    int next = 0;
    for (const auto line : split_lines(alias)) {
        const std::string trimmed = trim_copy(line);
        if (!starts_with(trimmed, "[Object.") || trimmed.back() != ']') continue;
        int index = -1;
        const auto number = std::string_view(trimmed).substr(8, trimmed.size() - 9);
        const auto result = std::from_chars(number.data(), number.data() + number.size(), index);
        if (result.ec == std::errc{} && index >= next) next = index + 1;
    }
    return next;
}

std::string effect_block_body(std::string_view effect_block) {
    std::string body;
    for (const auto line : split_lines(effect_block)) {
        if (line.empty() || is_section_header(line)) continue;
        body.append(line);
        body.push_back('\n');
    }
    return body;
}

std::optional<std::string> append_effect_to_alias(std::string_view alias, const Slot& slot) {
    if (slot.effect_block.empty()) return std::nullopt;
    std::string body = effect_block_body(slot.effect_block);
    if (body.empty()) return std::nullopt;

    std::string result(alias);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    result.push_back('\n');
    result += "[Object.";
    result += std::to_string(next_object_section_index(alias));
    result += "]\n";
    result += body;
    return result;
}

void add_slot_effect_to_object(EDIT_SECTION* edit, OBJECT_HANDLE object, const Slot& slot) {
    const LPCSTR alias = edit->get_object_alias(object);
    if (!alias) return;
    const auto modified_alias = append_effect_to_alias(alias, slot);
    if (!modified_alias) return;

    const std::string original_alias(alias);
    const OBJECT_LAYER_FRAME layer_frame = edit->get_object_layer_frame(object);
    edit->delete_object(object);
    OBJECT_HANDLE created = edit->create_object_from_alias(modified_alias->c_str(),
                                                           layer_frame.layer, layer_frame.start, 0);
    if (!created) {
        created = edit->create_object_from_alias(original_alias.c_str(), layer_frame.layer, layer_frame.start, 0);
    }
    if (created) edit->set_focus_object(created);
}

void register_slot_from_effect(EDIT_SECTION* edit, int slot_index, OBJECT_HANDLE object, LPCWSTR effect) {
    if (!object || !effect || slot_index < 0 || slot_index >= kSlotCount) return;
    const LPCSTR alias = edit->get_object_alias(object);
    if (!alias) return;
    const std::string effect_utf8 = wide_to_utf8(effect);
    const auto block = capture_effect_block(alias, effect_utf8);
    if (!block) return;
    g_slots[slot_index] = Slot{effect, effect_utf8, *block};
    save_slots_file();
    refresh_buttons();
}

void apply_slot_to_selection(EDIT_SECTION* edit, int slot_index) {
    if (slot_index < 0 || slot_index >= kSlotCount) return;
    const Slot& slot = g_slots[slot_index];
    if (slot.effect_name.empty()) return;
    OBJECT_HANDLE object = selected_or_focus_object(edit);
    if (!object) return;
    if (edit->count_object_effect(object, slot.effect_name.c_str()) <= 0) {
        add_slot_effect_to_object(edit, object, slot);
        return;
    }

    bool changed = false;
    for (const auto& item : captured_items(slot)) {
        if (item.name.empty()) continue;
        changed = edit->set_object_item_value(object, slot.effect_name.c_str(),
                                              item.name.c_str(), item.value.c_str()) || changed;
    }
    (void)changed;
}

void apply_slot(int slot_index) {
    if (!g_edit_handle) return;
    g_edit_handle->call_edit_section_param(reinterpret_cast<void*>(static_cast<intptr_t>(slot_index)),
        [](void* param, EDIT_SECTION* edit) {
            apply_slot_to_selection(edit, static_cast<int>(reinterpret_cast<intptr_t>(param)));
        });
}

void project_load(PROJECT_FILE* project) {
    const LPCSTR text = project->get_param_string(kProjectKey);
    if (fs::exists(slots_path())) {
        load_slots_file();
    } else if (text) {
        deserialize_slots(text);
        save_slots_file();
    }
    refresh_buttons();
}

void project_save(PROJECT_FILE* project) {
    const std::string text = serialize_slots();
    project->set_param_string(kProjectKey, text.c_str());
    save_slots_file();
}

void register_menu_item(void* param, OBJECT_HANDLE object, LPCWSTR effect, LPCWSTR) {
    const int slot_index = static_cast<int>(reinterpret_cast<intptr_t>(param));
    if (!g_edit_handle) return;
    struct MenuParam {
        int slot_index;
        OBJECT_HANDLE object;
        std::wstring effect;
    } menu_param{slot_index, object, effect ? effect : L""};

    g_edit_handle->call_edit_section_param(&menu_param, [](void* raw, EDIT_SECTION* edit) {
        auto* captured = static_cast<MenuParam*>(raw);
        register_slot_from_effect(edit, captured->slot_index, captured->object, captured->effect.c_str());
    });
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            for (int i = 0; i < kSlotCount; ++i) {
                CreateWindowExW(0, WC_BUTTONW, std::to_wstring(i + 1).c_str(),
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_MULTILINE | BS_OWNERDRAW,
                                0, 0, kButtonWidth, kButtonHeight, hwnd,
                                control_id(kButtonBase + i), g_instance, nullptr);
            }
            refresh_buttons();
            update_scroll();
            apply_theme(hwnd);
            return 0;
        case WM_SIZE:
            update_scroll();
            return 0;
        case WM_HSCROLL: {
            SCROLLINFO info{sizeof(info), SIF_ALL};
            GetScrollInfo(hwnd, SB_HORZ, &info);
            int pos = info.nPos;
            switch (LOWORD(wparam)) {
                case SB_LINELEFT: pos -= 24; break;
                case SB_LINERIGHT: pos += 24; break;
                case SB_PAGELEFT: pos -= static_cast<int>(info.nPage); break;
                case SB_PAGERIGHT: pos += static_cast<int>(info.nPage); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: pos = info.nTrackPos; break;
                default: break;
            }
            set_scroll(pos);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            g_wheel_delta += GET_WHEEL_DELTA_WPARAM(wparam);
            const int steps = g_wheel_delta / WHEEL_DELTA;
            g_wheel_delta %= WHEEL_DELTA;
            if (steps != 0) set_scroll(g_scroll - steps * 48);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) >= kButtonBase && LOWORD(wparam) < kButtonBase + kSlotCount) {
                apply_slot(static_cast<int>(LOWORD(wparam) - kButtonBase));
                return 0;
            }
            break;
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (!item || item->CtlID < kButtonBase || item->CtlID >= kButtonBase + kSlotCount) break;
            const bool selected = (item->itemState & ODS_SELECTED) != 0;
            const bool focus = (item->itemState & ODS_FOCUS) != 0;
            RECT rect = item->rcItem;
            const COLORREF bg = g_dark_mode ? (selected ? kDarkButtonHot : kDarkButton)
                                            : GetSysColor(COLOR_BTNFACE);
            HBRUSH brush = CreateSolidBrush(bg);
            FillRect(item->hDC, &rect, brush);
            DeleteObject(brush);

            HPEN pen = CreatePen(PS_SOLID, 1, g_dark_mode ? kDarkBorder : GetSysColor(COLOR_BTNSHADOW));
            HGDIOBJ old_pen = SelectObject(item->hDC, pen);
            HGDIOBJ old_brush = SelectObject(item->hDC, GetStockObject(HOLLOW_BRUSH));
            Rectangle(item->hDC, rect.left, rect.top, rect.right, rect.bottom);
            SelectObject(item->hDC, old_brush);
            SelectObject(item->hDC, old_pen);
            DeleteObject(pen);

            const int slot_index = static_cast<int>(item->CtlID - kButtonBase);
            const std::wstring number_text = std::to_wstring(slot_index + 1);
            const std::wstring effect_text =
                slot_index >= 0 && slot_index < kSlotCount ? g_slots[slot_index].effect_name : L"";
            SetBkMode(item->hDC, TRANSPARENT);
            SetTextColor(item->hDC, g_dark_mode ? kDarkText : GetSysColor(COLOR_BTNTEXT));
            InflateRect(&rect, -4, -2);
            TEXTMETRICW metrics{};
            GetTextMetricsW(item->hDC, &metrics);
            const int line_height = static_cast<int>(metrics.tmHeight + metrics.tmExternalLeading + 4);
            const int gap = effect_text.empty() ? 0 : 2;
            const int total_height = line_height + (effect_text.empty() ? 0 : line_height + gap);
            const int text_area_height = static_cast<int>(rect.bottom - rect.top);
            int y = static_cast<int>(rect.top) + std::max(0, (text_area_height - total_height) / 2);

            RECT number_rect{rect.left, y, rect.right, y + line_height};
            DrawTextW(item->hDC, number_text.c_str(), -1, &number_rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            if (!effect_text.empty()) {
                RECT effect_rect{rect.left, y + line_height + gap, rect.right, y + line_height + gap + line_height};
                DrawTextW(item->hDC, effect_text.c_str(), -1, &effect_rect,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            if (focus) DrawFocusRect(item->hDC, &item->rcItem);
            return TRUE;
        }
        case WM_ERASEBKGND: {
            RECT rect{};
            GetClientRect(hwnd, &rect);
            FillRect(reinterpret_cast<HDC>(wparam), &rect,
                     g_dark_mode ? g_dark_background_brush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            return TRUE;
        }
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORSTATIC:
            if (g_dark_mode) {
                HDC dc = reinterpret_cast<HDC>(wparam);
                SetTextColor(dc, kDarkText);
                SetBkColor(dc, kDarkButton);
                return reinterpret_cast<LRESULT>(g_dark_button_brush);
            }
            break;
        case WM_HOTKEY:
            if (wparam >= kHotKeyBase && wparam < kHotKeyBase + kSlotCount) {
                apply_slot(static_cast<int>(wparam - kHotKeyBase));
                return 0;
            }
            break;
        case WM_DESTROY:
            for (int i = 0; i < kSlotCount; ++i) UnregisterHotKey(hwnd, kHotKeyBase + i);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void register_class() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpszClassName = kWindowClass;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = g_instance;
    wc.hbrBackground = g_dark_mode ? g_dark_background_brush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
}

COMMON_PLUGIN_TABLE g_plugin_table{
    kPluginName,
    L"short-cut-filter-effect-plugin version 0.1.0"
};

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_instance = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

EXTERN_C __declspec(dllexport) DWORD RequiredVersion() {
    return 2003300;
}

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable() {
    return &g_plugin_table;
}

EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    g_dark_mode = system_uses_dark_mode();
    g_dark_background_brush = CreateSolidBrush(kDarkBackground);
    g_dark_button_brush = CreateSolidBrush(kDarkButton);
    return true;
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
    if (g_dark_background_brush) DeleteObject(g_dark_background_brush);
    if (g_dark_button_brush) DeleteObject(g_dark_button_brush);
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
    register_class();
    load_slots_file();
    g_main = CreateWindowExW(0, kWindowClass, kWindowName, WS_POPUP | WS_HSCROLL,
                             CW_USEDEFAULT, CW_USEDEFAULT, 360, 98,
                             nullptr, nullptr, g_instance, nullptr);
    if (!g_main) return;

    host->register_window_client(kWindowName, g_main);
    g_edit_handle = host->create_edit_handle();
    host->register_project_load_handler(project_load);
    host->register_project_save_handler(project_save);

    for (int i = 0; i < kSlotCount; ++i) {
        g_menu_names[i] = L"フィルタ効果をショートカットプラグインに登録\\" + std::to_wstring(i + 1);
        host->register_object_item_menu_param(g_menu_names[i].c_str(), true,
            reinterpret_cast<void*>(static_cast<intptr_t>(i)), register_menu_item);
        const UINT key = i == 9 ? '0' : static_cast<UINT>('1' + i);
        RegisterHotKey(g_main, kHotKeyBase + i, MOD_CONTROL, key);
    }
}
