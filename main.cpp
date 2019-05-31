#include <string>
#include <memory>
#include <vector>
#include <algorithm>
#include <optional>
#include <windows.h>
#include "resource.h"

void reportError(const std::wstring &message)
{
	MessageBox(NULL, message.c_str(), L"Error", MB_OK);
}

class Window
{
private:
	HINSTANCE hInstance_ = NULL;
	HWND hwnd_ = NULL;
public:
	HINSTANCE getInstanceHandle() const { return hInstance_; }
	HWND getWindowHandle() const { return hwnd_; }

	Window(HINSTANCE hInstance, int nCmdShow, LPCWSTR lpName, HICON hIcon)
	{
		create(hInstance, nCmdShow, lpName, hIcon);
	}

	~Window()
	{
		if (hwnd_) {
			// Detach this from window(hwnd_). Do not callback non static wndProc anymore.
			::SetWindowLongPtr(hwnd_, GWL_USERDATA, NULL);
		}
	}

	void create(HINSTANCE hInstance, int nCmdShow, LPCWSTR lpName, HICON hIcon)
	{
		if (!hwnd_) {
			WNDCLASSEX wc = { sizeof(WNDCLASSEX), 0, wndProcStatic, 0, 0, hInstance, hIcon, LoadCursor(NULL, IDC_ARROW) };
			wc.lpszClassName = L"MainWindow";
			RegisterClassEx(&wc);

			HWND hwnd = CreateWindow(L"MainWindow", lpName, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hInstance, this); // Call WM_CREATE

			ShowWindow(hwnd, nCmdShow);
			UpdateWindow(hwnd);

			hInstance_ = hInstance;
			hwnd_ = hwnd;
		}
	}

	void destroy()
	{
		if (hwnd_) {
			DestroyWindow(hwnd_); // Call WM_DESTROY, WM_NCDESTROY, hwnd_ = null
		}
	}

	static LRESULT CALLBACK wndProcStatic(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
	{
		if (msg == WM_CREATE) { // First chance to use hwnd
			// Attach this to window(hwnd)
			::SetWindowLongPtr(hwnd, GWL_USERDATA, reinterpret_cast<LONG_PTR>((reinterpret_cast<CREATESTRUCT *>(lparam))->lpCreateParams));
		}
		if (Window *pThis = reinterpret_cast<Window *>(GetWindowLongPtr(hwnd, GWL_USERDATA))) {
			return pThis->wndProc(hwnd, msg, wparam, lparam);
		}
		else {
			return DefWindowProc(hwnd, msg, wparam, lparam);

		}
	}

	LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
	{
		if (auto result = dispatchToHooks(hwnd, msg, wparam, lparam)) {
			return *result;
		}

		switch (msg) {
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		case WM_NCDESTROY: //Last chance to use hwnd
			hwnd_ = NULL;
			break;
		case WM_CREATE:
			return 0;
		case WM_COMMAND:
			return onCommand(hwnd, (int)LOWORD(wparam), (HWND)(lparam), (UINT)HIWORD(wparam));
		}
		return DefWindowProc(hwnd, msg, wparam, lparam);
	}

	int onCommand(HWND hwnd, int id, HWND hwndctl, UINT codeNotify)
	{
		switch (id) {
		case ID_MENU_EXIT:
			destroy();
			break;
		}

		return TRUE;
	}

	// Window Message Hook

	class Hook
	{
	public:
		virtual ~Hook() {}
		virtual std::optional<LRESULT> WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) = 0;
	};
private:
	std::vector<Hook *> hooks_;
	int dispatchingCount_ = 0;
	int removedHookCount_ = 0;
public:
	void addHook(Hook *hook)
	{
		// Insert after dispatchingSize.
		if(hook){
			hooks_.push_back(hook);
		}
	}
	void removeHook(Hook *hook)
	{
		// Do not move index
		if (hook) {
			auto it = std::find(hooks_.begin(), hooks_.end(), hook);
			if (it != hooks_.end()) {
				*it = nullptr;
				++removedHookCount_;
			}
		}
	}
private:
	std::optional<LRESULT> dispatchToHooks(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
	{
		// Note: This function is reentrant because SendMessage will immediately calls WndProc.
		++dispatchingCount_;
		for (std::size_t i = 0, dispatchingSize = hooks_.size(); i < dispatchingSize; ++i) {
			Hook *hook = hooks_[i];
			if (hook) {
				std::optional<LRESULT> result = hook->WndProc(hwnd, msg, wparam, lparam);
				if (result) {
					return *result;
				}
			}
		}
		--dispatchingCount_;
		// Remove empty slots (care for recursive dispatching case)
		if (dispatchingCount_ == 0 && removedHookCount_ > 0) {
			hooks_.erase(std::remove(hooks_.begin(), hooks_.end(), nullptr), hooks_.end());
			removedHookCount_ = 0;
		}
		return std::nullopt;
	}
};


class NotifyIcon : private Window::Hook
{
	bool created_ = false;
	Window *window_ = NULL;
	HMENU hPopupMenu_ = NULL;
public:
	static const UINT WM_NOTIFY_ICON = WM_USER + 100;
	static const UINT WM_NOTIFY_ICON_BEFORE_OPEN_MENU = WM_USER + 101;
	static const UINT ID_TRAY_ICON = 0;

	NotifyIcon(Window *window, HICON hIcon, const WCHAR *pszTip)
	{
		create(window, hIcon, pszTip);
	}

	~NotifyIcon()
	{
		remove();
	}

	void create(Window *window, HICON hIcon, const WCHAR *pszTip) {
		if (!created_) {
			window->addHook(this);
			window_ = window;

			// Add icon to task tray
			NOTIFYICONDATA notifyIconData = {
				sizeof(NOTIFYICONDATA),
				window->getWindowHandle(),
				ID_TRAY_ICON, //uId
				NIF_MESSAGE | NIF_ICON | NIF_TIP, //uFlags
				WM_NOTIFY_ICON, //uCallbackMessage
				hIcon,
				L"TrayRun" //szTip
			};
			if (pszTip) {
				lstrcpyn(notifyIconData.szTip, pszTip, sizeof(notifyIconData.szTip) / sizeof(notifyIconData.szTip[0]));
			}
			Shell_NotifyIcon(NIM_ADD, &notifyIconData);

			// Create popup menu
			hPopupMenu_ = LoadMenu(window->getInstanceHandle(), MAKEINTRESOURCE(IDR_POPUPMENU));

			created_ = true;
		}
	}

	void remove() {
		if (created_) {
			created_ = false;

			// Remove icon from task tray
			NOTIFYICONDATA notifyIconData = {
				sizeof(NOTIFYICONDATA),
				window_->getWindowHandle(),
				ID_TRAY_ICON, //uId
			};
			Shell_NotifyIcon(NIM_DELETE, &notifyIconData);

			// Destroy popup menu
			DestroyMenu(hPopupMenu_);

			window_->removeHook(this);
			window_ = nullptr;
		}
	}

private:
	virtual std::optional<LRESULT> WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override
	{
		switch (msg) {
		case WM_DESTROY:
			remove();
			return std::nullopt;

		case WM_NOTIFY_ICON:
			onNotifyIcon(hwnd, wparam, lparam);
			return std::nullopt;
		}
		return std::nullopt;
	}

	void onNotifyIcon(HWND hwnd, WPARAM wparam, LPARAM lparam)
	{
		switch (lparam) {
		case WM_LBUTTONDOWN:
		case WM_RBUTTONDOWN:
			POINT pt;
			GetCursorPos(&pt);

			HMENU hMenu = GetSubMenu(hPopupMenu_, 0);
			SendMessage(hwnd, WM_NOTIFY_ICON_BEFORE_OPEN_MENU, 0, reinterpret_cast<LPARAM>(hMenu));
			TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, pt.x, pt.y, 0, hwnd, NULL);
			break;
		}
	}
};


HWND findProcessWindow(DWORD procId)
{
	struct IdWnd { DWORD procId; HWND hwnd; };
	IdWnd idWnd = { procId, NULL };
	EnumWindows(
		[](HWND hwnd, LPARAM lparam) {
			IdWnd *idwnd = reinterpret_cast<IdWnd *>(lparam);
			DWORD procId = 0;
			GetWindowThreadProcessId(hwnd, &procId);
			if (procId == idwnd->procId) {
				idwnd->hwnd = hwnd;
				return FALSE;
			}
			return TRUE;
		},
		reinterpret_cast<LPARAM>(&idWnd));
	return idWnd.hwnd;

}


class ChildProcessController : private Window::Hook
{
	Window * const window_;
	PROCESS_INFORMATION childProcessInfo_ = {};
	HWND childWnd_ = NULL;
public:
	ChildProcessController(Window *window, const std::wstring &cmd)
		: window_(window)
	{
		window_->addHook(this);
		createChildProcess(cmd);
	}
	~ChildProcessController()
	{
		window_->removeHook(this);
	}
	HANDLE getChildProcessHandle() const { return childProcessInfo_.hProcess; }

private:
	void createChildProcess(const std::wstring &cmd)
	{
		// Create child process
		std::wstring cmdNotConst(cmd);

		STARTUPINFO startupInfo = { sizeof(STARTUPINFO) };
		PROCESS_INFORMATION childProcessInfo = {};
		if (!CreateProcess(NULL, cmdNotConst.data(), NULL, NULL, FALSE,
			0,
			NULL,
			NULL,
			&startupInfo,
			&childProcessInfo)) {
			reportError(L"Failed to execute child process.");
			window_->destroy();
			return;
		}
		childProcessInfo_ = childProcessInfo;

		// Find child process's window
		///@todo use timer?
		HWND childWnd;
		for (int i = 0; i < 20; ++i) {
			Sleep(100);
			if (childWnd = findProcessWindow(childProcessInfo.dwProcessId)) {
				break;
			}
		}
		if (!childWnd) {
			reportError(L"Cannot find child process's window.");
			window_->destroy();
			return;
		}
		childWnd_ = childWnd;

		// Hide child window first
		hideChildWindow();
	}

	void hideChildWindow()
	{
		if(childWnd_){
			ShowWindow(childWnd_, SW_HIDE);
		}
	}
	void showChildWindow()
	{
		if(childWnd_){
			ShowWindow(childWnd_, SW_SHOW);
		}
	}

	virtual std::optional<LRESULT> WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override
	{
		switch (msg) {
		case WM_DESTROY:
			// Show child window for failing to close child process.
			showChildWindow();
			// Try to close child process.
			if(childWnd_){
				PostMessage(childWnd_, WM_CLOSE, 0, 0);
			}
			return std::nullopt;

		case NotifyIcon::WM_NOTIFY_ICON_BEFORE_OPEN_MENU:
			EnableMenuItem(reinterpret_cast<HMENU>(lparam), ID_MENU_SHOW, childWnd_ && IsWindow(childWnd_) && !IsWindowVisible(childWnd_) ? MF_ENABLED : MF_GRAYED);
			EnableMenuItem(reinterpret_cast<HMENU>(lparam), ID_MENU_HIDE, childWnd_ && IsWindow(childWnd_) && IsWindowVisible(childWnd_) ? MF_ENABLED : MF_GRAYED);
			return std::nullopt;
		case WM_COMMAND:
			switch (LOWORD(wparam)) {
			case ID_MENU_SHOW: showChildWindow(); return TRUE;
			case ID_MENU_HIDE: hideChildWindow(); return TRUE;
			}
			return std::nullopt;
		}
		return std::nullopt;
	}
};



int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
	// Parse command line arguments
	std::wstring title = L"TrayRun";
	std::wstring icon = L"";
	std::wstring command = L"cmd.exe";
	{
		struct Option {
			std::vector<const wchar_t *> switches;
			std::wstring *varPtr;
			const wchar_t *description;
		};
		Option options[] = {
			Option{{L"title"}, &title, L"Application name"},
			Option{{L"icon"}, &icon, L"Icon filename"},
			Option{{L"command", L"cmd", L"c"}, &command, L"Command"}
		};

		int argc = 0;
		LPWSTR * const argv = CommandLineToArgvW(lpCmdLine, &argc);

		for (int i = 0; i < argc; ++i) {
			const LPCWSTR arg = argv[i];
			if (arg[0] == L'/' || arg[0] == L'-') {
				auto optIt = std::find_if(std::begin(options), std::end(options), [&arg](const Option &opt) {
					return std::find_if(opt.switches.begin(), opt.switches.end(), [&arg](const wchar_t *optSw) {
						return lstrcmpi(optSw, arg + 1) == 0; }) != opt.switches.end();
				});
				if (optIt != std::end(options)) {
					if (optIt->varPtr) {
						if (i + 1 >= argc) {
							reportError(std::wstring(L"Specify value of ") + arg + L" option.");
							return 0;
						}
						++i;
						optIt->varPtr->assign(argv[i]);
					}
				}
				else {
					reportError(std::wstring(L"Unknown option ") + arg + L" specified.");
					return 0;
				}
			}
			else {
				reportError(std::wstring(L"Unknown option ") + arg + L" specified.");
				return 0;
			}
		}
		GlobalFree(argv);
	}


	// Load Icon
	const HICON hIconLoaded = icon.empty() ? NULL : (HICON)LoadImage(NULL, icon.c_str(), IMAGE_ICON, 16, 16, LR_LOADFROMFILE | LR_SHARED);
	const HICON hIcon = hIconLoaded ? hIconLoaded : LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP));

	// Create main window
	Window window(hInstance, SW_HIDE, title.c_str(), hIcon); //Always hide main window

	// Add notify icon to task stay
	NotifyIcon notifyIcon(&window, hIcon, title.c_str());

	// Create child process
	ChildProcessController childProcess(&window, command);

	// Message loop and Observe child process.
	MSG msg;
	for(;;){
		// Wait until child process finished or new window messages arrive.
		HANDLE childProcessHandle = childProcess.getChildProcessHandle();
		const int signal = MsgWaitForMultipleObjects(1, &childProcessHandle, FALSE, INFINITE, QS_ALLINPUT);

		if (signal == WAIT_OBJECT_0) { // Child process is destroyed
			window.destroy();
		}

		while (::PeekMessage(&msg, NULL, 0, 0, FALSE)) {
			if (!GetMessage(&msg, NULL, 0, 0)) {
				return msg.wParam;
			}
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}
}
