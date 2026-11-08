#include "share_window.h"
#include "share_bridge.h"
#include "common.h"
#include "config_dialog.h"
#include "secure_assets.h"
#include <shlwapi.h>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-pragmas"
#endif
#include "../../third_party/webview2/sdk/build/native/include/WebView2.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "webview2_uuid_compat.h"
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace la {
namespace {
constexpr wchar_t kClass[]=L"TS3LocalAudioShareWindow2";
constexpr wchar_t kOrigin[]=L"https://ts3share.local/";
constexpr UINT kDrain=WM_APP+41;
#ifdef LA_SHARE_TEST
constexpr UINT kScriptTest=WM_APP+42;
#endif
std::mutex windowMutex,queueMutex;
std::thread windowThread;
std::atomic<HWND> windowHandle{nullptr};
std::atomic<bool> windowOpen{false},windowClosing{false};
std::wstring configDirectory;
struct Pending {std::string json;bool pcm=false;};
std::deque<Pending> outgoing;
size_t queueBytes=0;
HMODULE loaderModule=nullptr;
HANDLE loaderReadLock=INVALID_HANDLE_VALUE;
std::shared_ptr<const ShareAssetBundle> verifiedAssets;
using CreateEnvironmentFn=HRESULT(STDAPICALLTYPE*)(PCWSTR,PCWSTR,ICoreWebView2EnvironmentOptions*,ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
using RuntimeVersionFn=HRESULT(STDAPICALLTYPE*)(PCWSTR,LPWSTR*);
CreateEnvironmentFn createEnvironment=nullptr;

template<class T> class ComPtr {
    T* value=nullptr;
public:
    ComPtr()=default;explicit ComPtr(T* p):value(p){}
    ~ComPtr(){Reset();}
    ComPtr(const ComPtr& other):value(other.value){if(value)value->AddRef();}
    ComPtr(ComPtr&& other)noexcept:value(other.value){other.value=nullptr;}
    ComPtr& operator=(const ComPtr& other){if(this!=&other){Reset();value=other.value;if(value)value->AddRef();}return *this;}
    ComPtr& operator=(ComPtr&& other)noexcept{if(this!=&other){Reset();value=other.value;other.value=nullptr;}return *this;}
    void Reset(){if(value){value->Release();value=nullptr;}}
    T** Put(){Reset();return &value;}
    T* Get()const{return value;}
    T* operator->()const{return value;}
    explicit operator bool()const{return value!=nullptr;}
};
template<class I,class... Args> class Handler final:public I {
    std::atomic<ULONG> refs{1};std::function<HRESULT(Args...)> function;
public:
    explicit Handler(std::function<HRESULT(Args...)> fn):function(std::move(fn)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override {
        if(!out)return E_POINTER;*out=nullptr;
        if(id==__uuidof(IUnknown)||id==__uuidof(I)){*out=static_cast<I*>(this);AddRef();return S_OK;}return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{ULONG n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Invoke(Args... args)override{try{return function(args...);}catch(...){return E_FAIL;}}
};
template<class I,class... Args,class F> ComPtr<I> Callback(F&& fn){return ComPtr<I>(new Handler<I,Args...>(std::forward<F>(fn)));}
struct WindowState {
    HWND hwnd=nullptr;bool closed=false,ready=false,fallback=false,fullscreen=false;
    LONG_PTR normalStyle=0,normalExStyle=0;WINDOWPLACEMENT normalPlacement{};
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    std::wstring assets;
    std::shared_ptr<const ShareAssetBundle> resources;
    ~WindowState()=default;
};
std::shared_ptr<WindowState> state;
HINSTANCE Module() {
    HMODULE module=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&windowOpen),&module);return module;
}
std::wstring ModuleDirectory() {
    wchar_t path[32768]={};if(!GetModuleFileNameW(Module(),path,32768))return {};
    std::wstring result=path;return result.substr(0,result.find_last_of(L"\\/"));
}
bool Local(const wchar_t* uri) {
    return uri&&wcsncmp(uri,kOrigin,wcslen(kOrigin))==0;
}
bool SourceLocal(ICoreWebView2* view) {
    LPWSTR uri=nullptr;bool okay=view&&SUCCEEDED(view->get_Source(&uri))&&Local(uri);CoTaskMemFree(uri);return okay;
}
void Fullscreen(const std::shared_ptr<WindowState>& s,bool enabled) {
    if(!s||!s->hwnd||s->fullscreen==enabled)return;
    if(enabled) {
        s->normalStyle=GetWindowLongPtrW(s->hwnd,GWL_STYLE);
        s->normalExStyle=GetWindowLongPtrW(s->hwnd,GWL_EXSTYLE);
        s->normalPlacement.length=sizeof(WINDOWPLACEMENT);
        if(!GetWindowPlacement(s->hwnd,&s->normalPlacement))return;
        MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
        if(!GetMonitorInfoW(MonitorFromWindow(s->hwnd,MONITOR_DEFAULTTONEAREST),&monitor))return;
        s->fullscreen=true;
        SetWindowLongPtrW(s->hwnd,GWL_STYLE,s->normalStyle&~(LONG_PTR)WS_OVERLAPPEDWINDOW);
        SetWindowLongPtrW(s->hwnd,GWL_EXSTYLE,s->normalExStyle&~(LONG_PTR)(WS_EX_DLGMODALFRAME|WS_EX_WINDOWEDGE|WS_EX_CLIENTEDGE|WS_EX_STATICEDGE));
        SetWindowPos(s->hwnd,HWND_TOP,monitor.rcMonitor.left,monitor.rcMonitor.top,
            monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top,
            SWP_FRAMECHANGED|SWP_NOOWNERZORDER);
    } else {
        s->fullscreen=false;
        SetWindowLongPtrW(s->hwnd,GWL_STYLE,s->normalStyle);
        SetWindowLongPtrW(s->hwnd,GWL_EXSTYLE,s->normalExStyle);
        SetWindowPlacement(s->hwnd,&s->normalPlacement);
        SetWindowPos(s->hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    }
    if(s->controller){RECT bounds{};GetClientRect(s->hwnd,&bounds);s->controller->put_Bounds(bounds);s->controller->NotifyParentWindowPositionChanged();}
}
void CloseState(const std::shared_ptr<WindowState>& s) {
    if(!s||s->closed)return;Fullscreen(s,false);s->closed=true;s->ready=false;
    ShareBridgeWindowClosed();ShareBridgeSetEmitter({});
    if(s->controller)s->controller->Close();s->view.Reset();s->controller.Reset();s->environment.Reset();
}
void Fallback(const std::shared_ptr<WindowState>& s,const char* reason) {
    Log("WebView2 unavailable: %s; native audio fallback",reason);
    s->fallback=true;if(s->hwnd)PostMessageW(s->hwnd,WM_CLOSE,0,0);
}
void Resize(const std::shared_ptr<WindowState>& s) {
    if(!s||!s->controller||s->closed)return;RECT bounds{};GetClientRect(s->hwnd,&bounds);s->controller->put_Bounds(bounds);
    s->controller->NotifyParentWindowPositionChanged();
}
void Drain(const std::shared_ptr<WindowState>& s) {
    if(!s||!s->view||!s->ready||s->closed||!SourceLocal(s->view.Get()))return;
    std::deque<Pending> batch;
    {std::lock_guard<std::mutex> lock(queueMutex);batch.swap(outgoing);queueBytes=0;}
    for(const auto& message:batch){auto json=Utf8ToWide(message.json);s->view->PostWebMessageAsJson(json.c_str());}
}
void ConfigureView(const std::shared_ptr<WindowState>& s) {
    std::weak_ptr<WindowState> weak=s;EventRegistrationToken token{};
    ComPtr<ICoreWebView2Settings> settings;
    if(SUCCEEDED(s->view->get_Settings(settings.Put()))&&settings) {
        settings->put_IsScriptEnabled(TRUE);settings->put_IsWebMessageEnabled(TRUE);
        settings->put_AreHostObjectsAllowed(FALSE);settings->put_AreDevToolsEnabled(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);settings->put_IsZoomControlEnabled(FALSE);
    }
    auto nav=Callback<ICoreWebView2NavigationStartingEventHandler,ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs*>(
        [](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* args){LPWSTR uri=nullptr;args->get_Uri(&uri);if(!Local(uri))args->put_Cancel(TRUE);CoTaskMemFree(uri);return S_OK;});
    s->view->add_NavigationStarting(nav.Get(),&token);s->view->add_FrameNavigationStarting(nav.Get(),&token);
    auto pop=Callback<ICoreWebView2NewWindowRequestedEventHandler,ICoreWebView2*,ICoreWebView2NewWindowRequestedEventArgs*>(
        [](ICoreWebView2*,ICoreWebView2NewWindowRequestedEventArgs* args){args->put_Handled(TRUE);return S_OK;});
    s->view->add_NewWindowRequested(pop.Get(),&token);
    auto permission=Callback<ICoreWebView2PermissionRequestedEventHandler,ICoreWebView2*,ICoreWebView2PermissionRequestedEventArgs*>(
        [](ICoreWebView2*,ICoreWebView2PermissionRequestedEventArgs* args){
            COREWEBVIEW2_PERMISSION_KIND kind{};args->get_PermissionKind(&kind);LPWSTR uri=nullptr;args->get_Uri(&uri);
            bool autoplay=Local(uri)&&kind==COREWEBVIEW2_PERMISSION_KIND_AUTOPLAY&&ShareBridgeMayPlayRemoteAudio();CoTaskMemFree(uri);
            args->put_State(autoplay?COREWEBVIEW2_PERMISSION_STATE_ALLOW:COREWEBVIEW2_PERMISSION_STATE_DENY);return S_OK;});
    s->view->add_PermissionRequested(permission.Get(),&token);
    auto received=Callback<ICoreWebView2WebMessageReceivedEventHandler,ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs*>(
        [weak](ICoreWebView2* view,ICoreWebView2WebMessageReceivedEventArgs* args){auto self=weak.lock();if(!self||self->closed||!SourceLocal(view))return S_OK;
            LPWSTR source=nullptr;args->get_Source(&source);bool local=Local(source);CoTaskMemFree(source);if(!local)return S_OK;
            LPWSTR json=nullptr;if(FAILED(args->get_WebMessageAsJson(&json))||!json)return S_OK;std::string text=WideToUtf8(json);CoTaskMemFree(json);if(text.size()>256*1024)return S_OK;
            // The bridge validates JSON size/depth/types before parsing. Receiving
            // a local object means the page has installed its message listener.
            if(!text.empty()&&text.front()=='{')self->ready=true;
            if(self->ready)ShareBridgeHandleMessage(text);Drain(self);return S_OK;});
    s->view->add_WebMessageReceived(received.Get(),&token);
    auto resource=Callback<ICoreWebView2WebResourceRequestedEventHandler,ICoreWebView2*,ICoreWebView2WebResourceRequestedEventArgs*>(
        [weak](ICoreWebView2*,ICoreWebView2WebResourceRequestedEventArgs* args){auto self=weak.lock();if(!self||self->closed)return S_OK;
            ComPtr<ICoreWebView2WebResourceRequest> request;if(FAILED(args->get_Request(request.Put())))return S_OK;LPWSTR uri=nullptr;request->get_Uri(&uri);
            const std::wstring address=uri?uri:L"";CoTaskMemFree(uri);
            // Blob media is produced by the local page. Every disk-backed resource
            // is an exact manifest URL, served from the already verified snapshot.
            if(address.rfind(L"blob:https://ts3share.local/",0)==0)return S_OK;
            ComPtr<ICoreWebView2WebResourceResponse> response;
            auto entry=self->resources?self->resources->find(address):ShareAssetBundle::const_iterator{};
            if(self->resources&&entry!=self->resources->end()) {
                LPWSTR method=nullptr;request->get_Method(&method);const bool get=method&&wcscmp(method,L"GET")==0;CoTaskMemFree(method);
                if(get){const auto& asset=entry->second;ComPtr<IStream> stream(SHCreateMemStream(asset.bytes.data(),static_cast<UINT>(asset.bytes.size())));
                    std::wstring headers=L"Content-Type: "+asset.mime+L"\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
                    if(entry->first==L"https://ts3share.local/index.html")headers+=L"Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; media-src 'self' blob:; connect-src 'self'; worker-src 'self'; object-src 'none'; frame-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n";
                    self->environment->CreateWebResourceResponse(stream.Get(),200,L"OK",headers.c_str(),response.Put());}
            }
            if(!response&&self->environment)self->environment->CreateWebResourceResponse(nullptr,403,L"Blocked by LocalAudio",L"Content-Type: text/plain\r\nCache-Control: no-store\r\n",response.Put());
            if(response)args->put_Response(response.Get());return S_OK;});
    ComPtr<ICoreWebView2_22> allSources;
    if(SUCCEEDED(s->view->QueryInterface(IID_ICoreWebView2_22,reinterpret_cast<void**>(allSources.Put())))&&allSources)
        allSources->AddWebResourceRequestedFilterWithRequestSourceKinds(L"*",COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL,COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL);
    else s->view->AddWebResourceRequestedFilter(L"*",COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    s->view->add_WebResourceRequested(resource.Get(),&token);
    auto fullscreen=Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler,ICoreWebView2*,IUnknown*>(
        [weak](ICoreWebView2* view,IUnknown*){auto self=weak.lock();if(!self||self->closed||!SourceLocal(view))return S_OK;
            BOOL contains=FALSE;if(SUCCEEDED(view->get_ContainsFullScreenElement(&contains)))Fullscreen(self,contains!=FALSE);return S_OK;});
    s->view->add_ContainsFullScreenElementChanged(fullscreen.Get(),&token);
    auto failed=Callback<ICoreWebView2ProcessFailedEventHandler,ICoreWebView2*,ICoreWebView2ProcessFailedEventArgs*>(
        [weak](ICoreWebView2*,ICoreWebView2ProcessFailedEventArgs*){if(auto self=weak.lock())Fallback(self,"browser process failed");return S_OK;});
    s->view->add_ProcessFailed(failed.Get(),&token);
    // Do not set a virtual-host folder mapping: WebView2 bypasses the
    // WebResourceRequested event for mapped hosts, which would read plaintext
    // disk files and leave encrypted releases blank. All resources must pass
    // through the verified in-memory response handler above.
    ComPtr<ICoreWebView2_4> downloads;
    if(SUCCEEDED(s->view->QueryInterface(__uuidof(ICoreWebView2_4),reinterpret_cast<void**>(downloads.Put())))&&downloads){
        auto download=Callback<ICoreWebView2DownloadStartingEventHandler,ICoreWebView2*,ICoreWebView2DownloadStartingEventArgs*>(
            [](ICoreWebView2*,ICoreWebView2DownloadStartingEventArgs* args){args->put_Cancel(TRUE);return S_OK;});
        downloads->add_DownloadStarting(download.Get(),&token);
    }
    ShareBridgeSetEmitter(ShareWindowPostJson);Resize(s);s->controller->put_IsVisible(TRUE);
    s->view->Navigate(L"https://ts3share.local/index.html");
}
void CreateView(const std::shared_ptr<WindowState>& s,const std::wstring& userData) {
    std::weak_ptr<WindowState> weak=s;
    auto complete=Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,HRESULT,ICoreWebView2Environment*>(
        [weak](HRESULT result,ICoreWebView2Environment* env){auto self=weak.lock();if(!self||self->closed)return S_OK;
            if(FAILED(result)||!env){Fallback(self,"environment creation failed");return S_OK;}
            env->AddRef();self->environment=ComPtr<ICoreWebView2Environment>(env);
            auto controllerComplete=Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,HRESULT,ICoreWebView2Controller*>(
                [weak](HRESULT created,ICoreWebView2Controller* controller){auto target=weak.lock();if(!target||target->closed){if(controller)controller->Close();return S_OK;}
                    if(FAILED(created)||!controller){Fallback(target,"controller creation failed");return S_OK;}
                    controller->AddRef();target->controller=ComPtr<ICoreWebView2Controller>(controller);
                    if(FAILED(controller->get_CoreWebView2(target->view.Put()))||!target->view){Fallback(target,"browser unavailable");return S_OK;}
                    ConfigureView(target);return S_OK;});
            if(FAILED(env->CreateCoreWebView2Controller(self->hwnd,controllerComplete.Get())))Fallback(self,"controller request failed");return S_OK;});
    if(FAILED(createEnvironment(nullptr,userData.c_str(),nullptr,complete.Get())))Fallback(s,"environment request failed");
}
LRESULT CALLBACK WindowProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    auto s=state;
    switch(msg){
        case WM_SIZE:if(s&&wp!=SIZE_MINIMIZED)Resize(s);return 0;
        case WM_MOVE:if(s&&s->controller)s->controller->NotifyParentWindowPositionChanged();return 0;
        case WM_GETMINMAXINFO:{if(!s||!s->fullscreen){auto info=reinterpret_cast<MINMAXINFO*>(lp);info->ptMinTrackSize.x=860;info->ptMinTrackSize.y=620;}return 0;}
        case kDrain:Drain(s);return 0;
        case WM_TIMER:if(s&&!s->closed&&s->ready){ShareBridgeTick();Drain(s);}return 0;
        case WM_SETFOCUS:if(s&&s->controller)s->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);return 0;
#ifdef LA_SHARE_TEST
        case kScriptTest:{auto script=reinterpret_cast<std::wstring*>(lp);if(script){if(s&&s->view&&SourceLocal(s->view.Get()))s->view->ExecuteScript(script->c_str(),nullptr);delete script;}return 0;}
#endif
        case WM_CLOSE:CloseState(s);DestroyWindow(hwnd);return 0;
        case WM_DESTROY:KillTimer(hwnd,1);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void ThreadMain(std::wstring assets,std::wstring userData) {
    HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(com)){windowOpen=false;ShowConfigDialog();return;}
    auto s=std::make_shared<WindowState>();s->assets=std::move(assets);s->resources=verifiedAssets;state=s;
    WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=Module();wc.lpfnWndProc=WindowProc;wc.lpszClassName=kClass;
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION);wc.hIconSm=wc.hIcon;
    RegisterClassExW(&wc);
    RECT rect{0,0,1180,780};AdjustWindowRectEx(&rect,WS_OVERLAPPEDWINDOW,FALSE,0);
    HWND hwnd=CreateWindowExW(WS_EX_APPWINDOW,kClass,L"LocalAudio · 屏幕版",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,
        rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,Module(),nullptr);
    s->hwnd=hwnd;windowHandle=hwnd;
    if(hwnd){
        if(windowClosing.load())PostMessageW(hwnd,WM_CLOSE,0,0);
        else {ShowWindow(hwnd,SW_SHOW);UpdateWindow(hwnd);SetTimer(hwnd,1,500,nullptr);CreateView(s,userData);}
        MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    }
    CloseState(s);windowHandle=nullptr;state.reset();UnregisterClassW(kClass,Module());
    {std::lock_guard<std::mutex> lock(queueMutex);outgoing.clear();queueBytes=0;}
    bool fallback=s->fallback;s.reset();CoUninitialize();windowOpen=false;if(fallback&&!windowClosing.load())ShowConfigDialog();
}
bool LoadRuntime(const std::wstring& assets) {
    (void)assets;
    if(!loaderModule){auto path=ModuleDirectory()+L"\\ts3localaudio\\WebView2Loader.dll";
        loaderReadLock=OpenVerifiedWebViewLoader(path);if(loaderReadLock==INVALID_HANDLE_VALUE)return false;
        loaderModule=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!loaderModule){CloseHandle(loaderReadLock);loaderReadLock=INVALID_HANDLE_VALUE;return false;}createEnvironment=reinterpret_cast<CreateEnvironmentFn>(GetProcAddress(loaderModule,"CreateCoreWebView2EnvironmentWithOptions"));}
    auto version=reinterpret_cast<RuntimeVersionFn>(GetProcAddress(loaderModule,"GetAvailableCoreWebView2BrowserVersionString"));
    if(!createEnvironment||!version)return false;LPWSTR text=nullptr;HRESULT hr=version(nullptr,&text);CoTaskMemFree(text);return SUCCEEDED(hr);
}
}
bool VerifyShareWindowInstallation() {
    const auto root=ModuleDirectory()+L"\\ts3localaudio";
    if(!LoadVerifiedShareAssets(root))return false;
    HANDLE loader=OpenVerifiedWebViewLoader(root+L"\\WebView2Loader.dll");
    if(loader==INVALID_HANDLE_VALUE)return false;CloseHandle(loader);return true;
}
void SetShareWindowConfigDirectory(const std::wstring& directory){std::lock_guard<std::mutex> lock(windowMutex);configDirectory=directory;}
bool ShowShareWindow() {
    std::lock_guard<std::mutex> lock(windowMutex);
    if(windowOpen.load()){HWND hwnd=windowHandle.load();if(hwnd){ShowWindow(hwnd,SW_RESTORE);SetForegroundWindow(hwnd);}return true;}
    if(windowThread.joinable())windowThread.join();
    const auto root=ModuleDirectory()+L"\\ts3localaudio";
    verifiedAssets=LoadVerifiedShareAssets(root);
    if(!verifiedAssets||!VerifyShareWindowInstallation()) {
        MessageBoxW(nullptr,L"LocalAudio 组件缺失或完整性校验失败。为保护音频与屏幕权限，本次已停止打开共享面板。请从可信来源重新安装。",L"LocalAudio · 组件校验失败",MB_OK|MB_ICONERROR);
        return true; // Handled failure: do not silently fall back to the old UI.
    }
    auto assets=root+L"\\ui";if(!LoadRuntime(assets))return false;
    HMODULE pinned=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&windowOpen),&pinned))return false;
    std::wstring userData=configDirectory+L"\\LocalAudioWebView";CreateDirectoryW(userData.c_str(),nullptr);
    windowClosing=false;windowOpen=true;
    try{windowThread=std::thread(ThreadMain,std::move(assets),std::move(userData));}catch(...){windowOpen=false;return false;}return true;
}
void CloseShareWindow() {
    std::lock_guard<std::mutex> lock(windowMutex);windowClosing=true;HWND hwnd=windowHandle.load();if(hwnd)PostMessageW(hwnd,WM_CLOSE,0,0);
    if(windowThread.joinable())windowThread.join();
}
void ShareWindowPostJson(std::string json) {
    if(!windowOpen.load()||windowClosing.load()||json.empty()||json.size()>256*1024)return;
    bool pcm=json.find("\"type\":\"pcm\"")!=std::string::npos;
    {std::lock_guard<std::mutex> lock(queueMutex);
        // PCM is disposable: cap queue latency/memory rather than stalling capture.
        if(pcm&&outgoing.size()>=20)return;
        while(!outgoing.empty()&&(outgoing.size()>=128||queueBytes+json.size()>1024*1024)){queueBytes-=outgoing.front().json.size();outgoing.pop_front();}
        queueBytes+=json.size();outgoing.push_back({std::move(json),pcm});}
    HWND hwnd=windowHandle.load();if(hwnd)PostMessageW(hwnd,kDrain,0,0);
}
#ifdef LA_SHARE_TEST
bool ShareWindowExecuteScriptForTest(const std::wstring& script){HWND hwnd=windowHandle.load();if(!hwnd)return false;auto copy=new std::wstring(script);if(!PostMessageW(hwnd,kScriptTest,0,reinterpret_cast<LPARAM>(copy))){delete copy;return false;}return true;}
#endif
}
