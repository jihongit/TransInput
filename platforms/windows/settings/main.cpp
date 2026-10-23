// SPDX-License-Identifier: AGPL-3.0-only
// Standalone current-user settings/tray application; never injected into apps.
#include "../translator/core.h"
#include "../translator/client.h"
#include "../translator/http.h"
#include "../translator/settings.h"
#include "../translator/request_gate.h"
#include "../shared/learning_control.h"
#include "../shared/words_control.h"
#include "../tsf/session_id.h"
#include <shellapi.h>
#include <gdiplus.h>
#include <mutex>
#ifdef TI_SETTINGS_FIXTURE
#include "../tests/words_fixture.h"
#include "../shared/client.h"
#include <cstdio>
#endif
namespace {
using namespace transinput;
constexpr wchar_t ClassName[]=L"TransInput.Settings.40e2b4b8";
constexpr UINT Tray=WM_APP+100,Completed=WM_APP+101,ShowSettings=WM_APP+102;
constexpr int EnableId=100,KeyId=101,TestId=102,SaveId=IDOK,ExitId=103,ShowId=104;
constexpr int TranslationPageId=105,LearningPageId=106,RememberId=107,ClearId=108,RefreshId=109;
constexpr int WordsPageId=110,WordTextId=111,WordPinyinId=112,WordListId=113,WordAddId=114,WordRemoveId=115,WordRefreshId=116,WordPreviousId=117,WordNextId=118;
struct Job {
    pipe::Handle stop{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    std::mutex mutex;std::wstring message;
    HWND window{};
    bool learning{};learning_control::Result learned;
    bool words{};uint32_t words_action{};words_control::Result vocabulary;
};
struct Task {std::shared_ptr<Job> job;translation::Secret key;bool enabled{},test{};uint32_t learning_action{},words_action{},words_offset{};words_control::Row word;std::wstring settings,core,executable,broker,words_endpoint;};
struct Application {
    HWND window{},heading{},subtitle{},enable{},label{},key{},notice{},status{},test{},save{};
    HWND translation_page{},learning_tab{},remember{},learning_notice{},learning_status{},clear{},refresh{};
    HWND words_tab{},word_label{},word_text{},pinyin_label{},word_pinyin{},words_notice{},word_list{},words_status{},word_add{},word_remove{},word_refresh{},word_previous{},word_next{};
    HFONT font{},title_font{};HICON icon{};
    HBRUSH background{CreateSolidBrush(RGB(246,248,253))};
    UINT dpi{96},taskbar_message{RegisterWindowMessageW(L"TaskbarCreated")};
    bool tray_added{},quitting{},fixture{},loading{true};
    bool learning_page{};
    bool words_page{},words_testing{};
    words_control::View words_view;
    std::wstring words_endpoint{pipe::Endpoint()};
    std::wstring settings_path,core_path,executable;
    std::wstring broker_executable;
    std::shared_ptr<Job> job;
    ~Application(){if(icon)DestroyIcon(icon);if(font)DeleteObject(font);if(title_font)DeleteObject(title_font);if(background)DeleteObject(background);}
    int Px(int value)const{return MulDiv(value,static_cast<int>(dpi),96);}
    void Layout(){
        dpi=GetDpiForWindow(window);if(!dpi)dpi=96;
        const auto previous=font,previous_title=title_font;
        font=CreateFontW(-Px(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
        title_font=CreateFontW(-Px(27),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        for(const auto control:{heading,subtitle,enable,label,key,notice,status,test,save,translation_page,learning_tab,remember,learning_notice,learning_status,clear,refresh})SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        for(const auto control:{words_tab,word_label,word_text,pinyin_label,word_pinyin,words_notice,word_list,words_status,word_add,word_remove,word_refresh,word_previous,word_next})SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        SendMessageW(heading,WM_SETFONT,reinterpret_cast<WPARAM>(title_font),TRUE);
        const auto place=[&](HWND control,int x,int y,int width,int height){SetWindowPos(control,nullptr,Px(x),Px(y),Px(width),Px(height),SWP_NOZORDER|SWP_NOACTIVATE);};
        place(heading,28,22,380,36);place(subtitle,28,62,380,24);
        place(translation_page,28,96,120,28);place(learning_tab,158,96,120,28);place(words_tab,288,96,120,28);place(enable,28,136,380,28);
        place(label,28,178,380,20);place(key,28,202,380,34);place(notice,28,251,380,52);
        place(status,28,310,380,44);place(test,184,370,108,36);place(save,304,370,104,36);
        place(remember,28,146,380,28);place(learning_notice,28,200,380,72);place(learning_status,28,296,380,64);
        place(refresh,184,370,108,36);place(clear,304,370,104,36);
        place(word_label,28,139,48,22);place(word_text,82,134,326,30);place(pinyin_label,28,179,48,22);place(word_pinyin,82,174,326,30);
        place(words_notice,28,211,380,38);place(word_list,28,254,380,68);place(words_status,28,328,380,34);
        place(word_previous,28,370,48,36);place(word_next,82,370,48,36);place(word_refresh,138,370,64,36);place(word_remove,210,370,88,36);place(word_add,306,370,102,36);
        if(previous)DeleteObject(previous);if(previous_title)DeleteObject(previous_title);
    }
    HWND Control(const wchar_t* type,const wchar_t* text,DWORD style,int id){
        const auto control=CreateWindowExW(wcscmp(type,L"EDIT")==0?WS_EX_CLIENTEDGE:0,type,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        if(!control)throw std::runtime_error("settings control");return control;
    }
    void Create(){
        heading=Control(L"STATIC",L"TransInput",0,0);subtitle=Control(L"STATIC",L"中文输入，自然美式英语",0,0);
        translation_page=Control(L"BUTTON",L"翻译",BS_AUTORADIOBUTTON|BS_PUSHLIKE|WS_TABSTOP|WS_GROUP,TranslationPageId);
        learning_tab=Control(L"BUTTON",L"中文输入",BS_AUTORADIOBUTTON|BS_PUSHLIKE|WS_TABSTOP,LearningPageId);
        words_tab=Control(L"BUTTON",L"个人词库",BS_AUTORADIOBUTTON|BS_PUSHLIKE|WS_TABSTOP,WordsPageId);
        enable=Control(L"BUTTON",L"启用 DeepSeek 翻译",BS_AUTOCHECKBOX|WS_TABSTOP,EnableId);
        label=Control(L"STATIC",L"API Key",0,0);key=Control(L"EDIT",L"",ES_PASSWORD|ES_AUTOHSCROLL|WS_TABSTOP,KeyId);
        SendMessageW(key,EM_SETLIMITTEXT,512,0);SendMessageW(key,EM_SETPASSWORDCHAR,L'●',0);
        notice=Control(L"STATIC",L"开启后，当前中文候选会发送给 DeepSeek，可能产生 API 费用。关闭翻译后仍可正常输入中文。",0,0);
        status=Control(L"STATIC",L"设置只保存在当前 Windows 账户。",0,0);
        test=Control(L"BUTTON",L"测试连接",BS_PUSHBUTTON|WS_TABSTOP,TestId);save=Control(L"BUTTON",L"保存",BS_DEFPUSHBUTTON|WS_TABSTOP,SaveId);
        remember=Control(L"BUTTON",L"记住我的中文选词",BS_AUTOCHECKBOX|WS_TABSTOP,RememberId);
        learning_notice=Control(L"STATIC",L"只在这台电脑记住你明确选择的中文。关闭后停止记录，已有偏好会保留；清空记录不会影响基础词库或翻译设置。更改开关会立即保存。",0,0);
        learning_status=Control(L"STATIC",fixture?L"已记住 12 条选词偏好。\r\n记录只保存在当前 Windows 账户。":L"打开此页后读取本地选词状态。",0,0);
        clear=Control(L"BUTTON",L"清空记录",BS_PUSHBUTTON|WS_TABSTOP,ClearId);refresh=Control(L"BUTTON",L"刷新状态",BS_PUSHBUTTON|WS_TABSTOP,RefreshId);
        word_label=Control(L"STATIC",L"词条",0,0);word_text=Control(L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,WordTextId);SendMessageW(word_text,EM_SETLIMITTEXT,64,0);
        pinyin_label=Control(L"STATIC",L"拼音",0,0);word_pinyin=Control(L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,WordPinyinId);SendMessageW(word_pinyin,EM_SETLIMITTEXT,128,0);
        words_notice=Control(L"STATIC",L"拼音用空格分隔可简拼，如 xing he → xh。词库仅本地保存，翻译会发送当前候选。",0,0);
        word_list=Control(L"LISTBOX",L"",LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL|WS_BORDER|WS_TABSTOP,WordListId);
        words_status=Control(L"STATIC",L"打开此页后读取个人词库。",0,0);
        word_add=Control(L"BUTTON",L"添加词条",BS_PUSHBUTTON|WS_TABSTOP,WordAddId);word_remove=Control(L"BUTTON",L"删除所选",BS_PUSHBUTTON|WS_TABSTOP,WordRemoveId);
        word_refresh=Control(L"BUTTON",L"刷新",BS_PUSHBUTTON|WS_TABSTOP,WordRefreshId);word_previous=Control(L"BUTTON",L"上页",BS_PUSHBUTTON|WS_TABSTOP,WordPreviousId);word_next=Control(L"BUTTON",L"下页",BS_PUSHBUTTON|WS_TABSTOP,WordNextId);
        SendMessageW(remember,BM_SETCHECK,BST_CHECKED,0);SwitchPage(false);
        Layout();
        if(!fixture){try{auto config=translation::LoadSettings(settings_path);SendMessageW(enable,BM_SETCHECK,config.enabled?BST_CHECKED:BST_UNCHECKED,0);
            std::wstring text;for(const auto byte:config.key.bytes)text.push_back(static_cast<wchar_t>(byte));SetWindowTextW(key,text.c_str());
            if(!text.empty())SecureZeroMemory(text.data(),text.size()*sizeof(wchar_t));
        }catch(...){SetWindowTextW(status,L"配置无法读取，请重新填写并保存。");}}
        if(fixture)SetWindowTextW(key,L"sk-demo-not-a-real-key");
        loading=false;
    }
    void SwitchPage(bool learning,bool words=false){
        learning_page=learning&&!words;words_page=words;learning=learning_page;
        SendMessageW(translation_page,BM_SETCHECK,learning||words?BST_UNCHECKED:BST_CHECKED,0);SendMessageW(learning_tab,BM_SETCHECK,learning?BST_CHECKED:BST_UNCHECKED,0);SendMessageW(words_tab,BM_SETCHECK,words?BST_CHECKED:BST_UNCHECKED,0);
        for(const auto control:{enable,label,key,notice,status,test,save})ShowWindow(control,learning||words?SW_HIDE:SW_SHOW);
        for(const auto control:{remember,learning_notice,learning_status,clear,refresh})ShowWindow(control,learning?SW_SHOW:SW_HIDE);
        for(const auto control:{word_label,word_text,pinyin_label,word_pinyin,words_notice,word_list,words_status,word_add,word_remove,word_refresh,word_previous,word_next})ShowWindow(control,words?SW_SHOW:SW_HIDE);
        SendMessageW(save,BM_SETSTYLE,learning||words?BS_PUSHBUTTON:BS_DEFPUSHBUTTON,TRUE);SendMessageW(refresh,BM_SETSTYLE,learning?BS_DEFPUSHBUTTON:BS_PUSHBUTTON,TRUE);
        SendMessageW(word_add,BM_SETSTYLE,words?BS_DEFPUSHBUTTON:BS_PUSHBUTTON,TRUE);
        SetWindowTextW(subtitle,words?L"自己的词条，保存在本机":learning?L"选词偏好只留在这台电脑":L"中文输入，自然美式英语");
        if(learning&&!fixture&&!loading&&!job)BeginLearning(wire::LearningStatus);
        if(words&&fixture&&!words_testing){words_view.total=2;words_view.health=words_control::Ready;words_view.rows={{"xing he yan jiu suo","星河研究所"},{"yun duo gong zuo shi","云朵工作室"}};DisplayWords(L"已保存 2 个词条 · 第 1/1 页");}
        else if(words&&!loading&&!job)BeginWords(wire::WordsStatus);
    }
    void TrayIcon(){
        if(fixture)return;
        NOTIFYICONDATAW data{sizeof data};data.hWnd=window;data.uID=1;data.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;data.uCallbackMessage=Tray;
        data.hIcon=icon?icon:LoadIconW(nullptr,IDI_APPLICATION);wcscpy_s(data.szTip,L"TransInput 设置");
        tray_added=Shell_NotifyIconW(NIM_ADD,&data)!=FALSE;
        if(tray_added){data.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&data);}
    }
    void RemoveTray(){if(tray_added){NOTIFYICONDATAW data{sizeof data};data.hWnd=window;data.uID=1;Shell_NotifyIconW(NIM_DELETE,&data);tray_added=false;}}
    void Show(){ShowWindow(window,SW_SHOWNORMAL);SetForegroundWindow(window);if(learning_page&&!job)BeginLearning(wire::LearningStatus);else if(words_page&&!job)BeginWords(wire::WordsStatus);}
    void Menu(){
        HMENU menu=CreatePopupMenu();if(!menu)return;AppendMenuW(menu,MF_STRING,ShowId,L"设置");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,ExitId,L"退出");
        POINT point{};GetCursorPos(&point);SetForegroundWindow(window);
        const auto selected=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window,nullptr);DestroyMenu(menu);
        if(selected==ShowId)Show();else if(selected==ExitId)Exit();
    }
    translation::Secret ReadKey(){
        const auto length=GetWindowTextLengthW(key);if(length<0||length>512)throw std::runtime_error("key length");
        std::wstring text(static_cast<size_t>(length)+1,L'\0');GetWindowTextW(key,text.data(),length+1);
        translation::Secret secret;bool valid=true;
        const auto space=[](wchar_t c){return c==L' '||c==L'\t'||c==L'\r'||c==L'\n'||c==0x3000;};
        int first{},last=length;while(first<last&&space(text[first]))++first;while(last>first&&space(text[last-1]))--last;
        for(int i=first;i<last;++i){if(text[i]<33||text[i]>126){valid=false;break;}secret.bytes.push_back(static_cast<uint8_t>(text[i]));}
        SecureZeroMemory(text.data(),text.size()*sizeof(wchar_t));if(!valid)throw std::runtime_error("key format");return secret;
    }
    void Busy(bool busy){for(const auto control:{enable,key,test,save,translation_page,learning_tab,remember,clear,refresh,words_tab,word_text,word_pinyin,word_list,word_add,word_remove,word_refresh,word_previous,word_next})EnableWindow(control,busy?FALSE:TRUE);if(!busy)WordButtons();}
    static std::wstring Wide(const std::string& bytes){
        if(bytes.empty())return {};const auto size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);if(!size)throw std::runtime_error("word UTF-8");
        std::wstring text(size,L'\0');if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),text.data(),size)!=size)throw std::runtime_error("word UTF-8");return text;
    }
    static std::string Field(HWND field){
        const auto length=GetWindowTextLengthW(field);if(length<0||length>128)throw std::runtime_error("word field length");std::wstring text(static_cast<size_t>(length)+1,L'\0');GetWindowTextW(field,text.data(),length+1);text.resize(length);
        const auto first=text.find_first_not_of(L" \t\r\n"),last=text.find_last_not_of(L" \t\r\n");if(first==std::wstring::npos)return {};text=text.substr(first,last-first+1);
        const auto size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);if(size<=0||size>128)throw std::runtime_error("word field encoding");std::string bytes(size,'\0');
        if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),bytes.data(),size,nullptr,nullptr)!=size)throw std::runtime_error("word field encoding");return bytes;
    }
    void WordButtons(){
        EnableWindow(word_add,words_view.health==words_control::Ready);
        EnableWindow(word_previous,words_view.offset>0);EnableWindow(word_next,words_view.offset+words_control::PageSize<words_view.total);
        const auto selected=SendMessageW(word_list,LB_GETCURSEL,0,0);EnableWindow(word_remove,words_view.health==words_control::Ready&&selected!=LB_ERR&&static_cast<size_t>(selected)<words_view.rows.size());
    }
    void DisplayWords(const std::wstring& message){
        SendMessageW(word_list,LB_RESETCONTENT,0,0);
        for(const auto& row:words_view.rows){const auto line=Wide(row.text)+L"  ·  "+Wide(row.pinyin);if(SendMessageW(word_list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(line.c_str()))<0)throw std::runtime_error("word list row");}
        SetWindowTextW(words_status,message.c_str());WordButtons();
    }
    void SelectWord(){
        const auto selected=SendMessageW(word_list,LB_GETCURSEL,0,0);if(selected!=LB_ERR&&static_cast<size_t>(selected)<words_view.rows.size()){
            const auto& row=words_view.rows[static_cast<size_t>(selected)];SetWindowTextW(word_text,Wide(row.text).c_str());SetWindowTextW(word_pinyin,Wide(row.pinyin).c_str());}WordButtons();
    }
    static std::wstring WordsMessage(const words_control::Result& result,uint32_t action){
        if(result.status||result.view.state==words_control::Unknown||result.view.state==words_control::Pending)return L"结果尚未确认，请刷新；不会自动重复添加或删除。";
        const auto& view=result.view;
        if(view.state==words_control::Failed){if(view.error==ERROR_INVALID_PARAMETER)return L"未保存：词条需含中文且不含空格；拼音只用字母，可用空格分隔。";
            if(view.error==ERROR_TOO_MANY_NAMES)return L"未保存。最多1024个词条，同一拼音最多32个。";return L"未完成更改，原词库保留。请检查文件访问权限。";}
        if(view.health==words_control::Loading)return L"正在读取词库，请稍后刷新。";
        if(view.health==words_control::ReadFailed)return L"词库无法读取，原文件已保留；基础中文仍可使用。";
        if(view.health==words_control::MemoryOnly)return L"当前为隔离模式，未连接可保存的个人词库。";
        return (action==wire::WordsAdd?L"已保存，新输入生效。":action==wire::WordsRemove?L"已删除所选词条。":L"")+std::wstring(L"共 ")+std::to_wstring(view.total)+L" 个 · 第 "+std::to_wstring(view.offset/words_control::PageSize+1)+L"/"+std::to_wstring(std::max(1u,(view.total+words_control::PageSize-1)/words_control::PageSize))+L" 页";
    }
    static bool LaunchBroker(const std::wstring& executable){
        auto command=L"\""+executable+L"\"";STARTUPINFOW startup{sizeof startup};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
        if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,pipe::ParentPath(executable).c_str(),&startup,&process))return false;
        CloseHandle(process.hProcess);CloseHandle(process.hThread);return true;
    }
    static std::wstring LearningMessage(const learning_control::Result& result,uint32_t action){
        if(result.status||result.view.state==learning_control::Unknown||result.view.state==learning_control::Pending)return L"操作结果尚未确认，请刷新状态；不会自动重复清空。";
        if(result.view.state==learning_control::Failed)return L"操作未完成，请检查当前账户的文件访问权限后重试。";
        const auto count=L"已记住 "+std::to_wstring(result.view.count)+L" 条选词偏好。\r\n";
        if(result.view.health==0)return L"正在读取本地选词数据，请稍后刷新。";
        if(result.view.health==3)return count+L"原记录无法读取，已保留原文件；当前仅在内存中记住选词。";
        if(result.view.health==4)return count+L"记录暂时无法保存，关闭程序可能丢失本次偏好。";
        if(result.view.health==5)return count+L"选词开关无法读取，当前不记录；请重新选择开关。";
        if(action==wire::LearningClear)return count+L"旧选词记录已清空，基础词库仍可使用。";
        return count+(result.view.enabled?L"正在记住选词；记录只保存在本机。":L"已停止记录，已有偏好仍可使用。");
    }
    static void CALLBACK Work(PTP_CALLBACK_INSTANCE instance,void* pointer)noexcept{
        std::unique_ptr<Task> task(static_cast<Task*>(pointer));CallbackMayRunLong(instance);std::wstring message;
        try{
            if(task->words_action){
                auto result=words_control::Query(task->words_endpoint,task->broker,task->job->stop.value,task->words_offset);
                if(result.status==ERROR_PIPE_NOT_CONNECTED&&task->words_endpoint==pipe::Endpoint()&&LaunchBroker(task->broker))result=words_control::Query(task->words_endpoint,task->broker,task->job->stop.value,task->words_offset);
                if(!result.status&&task->words_action!=wire::WordsStatus)result=words_control::Change(task->words_endpoint,task->broker,task->job->stop.value,task->words_action,NewSessionId(),task->word,task->words_offset);
                message=WordsMessage(result,task->words_action);task->job->vocabulary=std::move(result);
            }else if(task->learning_action){
                auto result=learning_control::Query(pipe::Endpoint(),task->broker,task->job->stop.value);
                if(result.status==ERROR_PIPE_NOT_CONNECTED&&LaunchBroker(task->broker))result=learning_control::Query(pipe::Endpoint(),task->broker,task->job->stop.value);
                if(!result.status&&task->learning_action!=wire::LearningStatus)result=learning_control::Change(pipe::Endpoint(),task->broker,task->job->stop.value,task->learning_action,NewSessionId(),task->enabled);
                message=LearningMessage(result,task->learning_action);task->job->learned=result;
            }else if(task->test){
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                try{
                    translation::RequestGate gate;
                    if(!gate.TryAcquire())message=L"正在处理其他翻译，请稍后测试。";
                    else{
                        translation::Core core(task->core);auto session=std::make_shared<translation::HttpSession>();
                        translation::Http http(session,core.Prompt(),"你好，很高兴认识你。",task->key.View());
                        while(!http.Poll(WaitForSingleObject(task->job->stop.value,0)==WAIT_OBJECT_0))Sleep(5);
                        switch(http.Result().failure){
                        case 0:message=L"连接成功，可保存设置。";break;
                        case 2:message=L"请求超时，请稍后重试。";break;
                        case 3:message=L"API Key 无效或没有访问权限。";break;
                        case 4:message=L"DeepSeek 暂时繁忙，请稍后重试。";break;
                        default:message=L"连接失败，请检查网络和 API Key。";break;
                        }
                    }
                }catch(...){winrt::uninit_apartment();throw;}
                winrt::uninit_apartment();
            }else{
                translation::SaveSettings(task->settings,task->enabled,task->key.View());
                translation::Query(translation::Endpoint(),task->executable,task->job->stop.value,translation::Reload,0,0,1);
                message=L"已保存，将从下一次中文组合生效。";
            }
        }catch(...){message=task->words_action?L"个人词库暂不可用，请稍后刷新。":task->learning_action?L"本地选词服务暂不可用，请稍后刷新。":task->test?L"无法完成测试，请检查网络和 API Key。":L"保存失败，请检查当前账户的文件访问权限。";}
        task->key.Clear();{std::lock_guard lock(task->job->mutex);task->job->message=std::move(message);}
        PostMessageW(task->job->window,Completed,0,0);
    }
    void BeginLearning(uint32_t action){
        if(job||fixture)return;
        try{
            auto task=std::make_unique<Task>();task->learning_action=action;task->broker=broker_executable;task->enabled=SendMessageW(remember,BM_GETCHECK,0,0)==BST_CHECKED;
            job=std::make_shared<Job>();job->window=window;job->learning=true;if(!job->stop)throw std::runtime_error("learning cancellation");task->job=job;
            Busy(true);SetWindowTextW(learning_status,action==wire::LearningStatus?L"正在读取本地选词状态…":L"正在保存更改…");
            if(!TrySubmitThreadpoolCallback(Work,task.get(),nullptr))throw std::runtime_error("learning settings worker");task.release();
        }catch(...){job.reset();Busy(false);SetWindowTextW(learning_status,L"本地选词服务暂不可用，请稍后刷新。");}
    }
    void BeginWords(uint32_t action,uint32_t offset=0){
        if(job||(fixture&&!words_testing))return;
        if(action!=wire::WordsStatus&&words_view.health!=words_control::Ready){SetWindowTextW(words_status,L"词库尚未就绪，请先刷新状态。");return;}
        try{
            auto task=std::make_unique<Task>();task->words_action=action;task->words_offset=offset;task->broker=broker_executable;task->words_endpoint=words_endpoint;
            if(action==wire::WordsAdd){task->word={Field(word_pinyin),Field(word_text)};if(task->word.pinyin.empty()||task->word.text.empty()){SetWindowTextW(words_status,L"请填写词条和拼音，例如“星河”和 xing he。");return;}}
            else if(action==wire::WordsRemove){const auto selected=SendMessageW(word_list,LB_GETCURSEL,0,0);if(selected==LB_ERR||static_cast<size_t>(selected)>=words_view.rows.size()){SetWindowTextW(words_status,L"请先选择要删除的词条。");return;}task->word=words_view.rows[static_cast<size_t>(selected)];task->words_offset=words_view.offset;}
            job=std::make_shared<Job>();job->window=window;job->words=true;job->words_action=action;task->job=job;if(!job->stop)throw std::runtime_error("word job cancellation");
            Busy(true);SetWindowTextW(words_status,action==wire::WordsStatus?L"正在读取词库…":L"正在保存更改…");
            if(!TrySubmitThreadpoolCallback(Work,task.get(),nullptr))throw std::runtime_error("word settings worker");task.release();
        }catch(...){job.reset();Busy(false);SetWindowTextW(words_status,L"未保存。词条最多32个字符；拼音只用英文字母和分隔符。");}
    }
    void Begin(bool testing){
        if(job||fixture)return;
        try{
            auto task=std::make_unique<Task>();task->key=ReadKey();task->enabled=SendMessageW(enable,BM_GETCHECK,0,0)==BST_CHECKED;
            if((testing||task->enabled)&&task->key.bytes.empty()){SetWindowTextW(status,L"请填写 API Key。");SetFocus(key);return;}
            task->test=testing;task->settings=settings_path;task->core=core_path;task->executable=executable;
            job=std::make_shared<Job>();job->window=window;if(!job->stop)throw std::runtime_error("settings cancellation event");task->job=job;
            Busy(true);SetWindowTextW(status,testing?L"正在测试连接…":L"正在保存…");
            if(!TrySubmitThreadpoolCallback(Work,task.get(),nullptr))throw std::runtime_error("settings worker");task.release();
        }catch(...){job.reset();Busy(false);SetWindowTextW(status,L"请输入有效的 API Key，或稍后重试。");}
    }
    void Done(){if(!job)return;std::wstring message;{std::lock_guard lock(job->mutex);message=job->message;}
        if(job->words){if(!job->vocabulary.status){words_view=std::move(job->vocabulary.view);DisplayWords(message);if(job->words_action==wire::WordsAdd&&words_view.state==words_control::Succeeded){SetWindowTextW(word_text,L"");SetWindowTextW(word_pinyin,L"");}}
            else SetWindowTextW(words_status,message.c_str());job.reset();Busy(false);if(quitting)DestroyWindow(window);return;}
        const bool learning=job->learning;if(learning&&!job->learned.status)SendMessageW(remember,BM_SETCHECK,job->learned.view.enabled?BST_CHECKED:BST_UNCHECKED,0);
        job.reset();Busy(false);SetWindowTextW(learning?learning_status:status,message.c_str());if(quitting)DestroyWindow(window);}
    void Exit(){quitting=true;ShowWindow(window,SW_HIDE);if(job)SetEvent(job->stop.value);else DestroyWindow(window);}
};
LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)noexcept{
    if(message==WM_NCCREATE){auto app=static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);app->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}
    auto app=reinterpret_cast<Application*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(!app)return DefWindowProcW(window,message,wparam,lparam);
    try{
        if(message==app->taskbar_message){app->TrayIcon();return 0;}
        switch(message){
        case WM_CREATE:app->Create();return 0;
        case WM_COMMAND:if(LOWORD(wparam)==SaveId){if(app->words_page)app->BeginWords(wire::WordsAdd);else if(app->learning_page)app->BeginLearning(wire::LearningStatus);else app->Begin(false);}else if(LOWORD(wparam)==TestId&&!app->learning_page&&!app->words_page)app->Begin(true);
            else if(LOWORD(wparam)==TranslationPageId)app->SwitchPage(false);else if(LOWORD(wparam)==LearningPageId)app->SwitchPage(true);
            else if(LOWORD(wparam)==WordsPageId)app->SwitchPage(false,true);
            else if(LOWORD(wparam)==WordAddId)app->BeginWords(wire::WordsAdd);
            else if(LOWORD(wparam)==WordRemoveId)app->BeginWords(wire::WordsRemove);
            else if(LOWORD(wparam)==WordRefreshId)app->BeginWords(wire::WordsStatus,app->words_view.offset);
            else if(LOWORD(wparam)==WordPreviousId)app->BeginWords(wire::WordsStatus,app->words_view.offset>=words_control::PageSize?app->words_view.offset-words_control::PageSize:0);
            else if(LOWORD(wparam)==WordNextId)app->BeginWords(wire::WordsStatus,std::min(TI_MAX_WORDS,app->words_view.offset+words_control::PageSize));
            else if(LOWORD(wparam)==WordListId&&HIWORD(wparam)==LBN_SELCHANGE)app->SelectWord();
            else if(LOWORD(wparam)==RememberId&&HIWORD(wparam)==BN_CLICKED)app->BeginLearning(wire::LearningEnable);
            else if(LOWORD(wparam)==RefreshId)app->BeginLearning(wire::LearningStatus);
            else if(LOWORD(wparam)==ClearId&&MessageBoxW(window,L"只清空你选择过的词条偏好，基础词库和翻译设置会保留。是否清空？",L"清空本地选词记录",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)==IDYES)app->BeginLearning(wire::LearningClear);
            else if(!app->loading&&!app->job&&((LOWORD(wparam)==KeyId&&HIWORD(wparam)==EN_CHANGE)||(LOWORD(wparam)==EnableId&&HIWORD(wparam)==BN_CLICKED)))SetWindowTextW(app->status,L"尚未保存。");return 0;
        case ShowSettings:app->Show();return 0;
        case Completed:app->Done();return 0;
        case Tray:if(LOWORD(lparam)==WM_RBUTTONUP||LOWORD(lparam)==WM_CONTEXTMENU)app->Menu();else if(LOWORD(lparam)==WM_LBUTTONUP||LOWORD(lparam)==NIN_SELECT)app->Show();return 0;
        case WM_DPICHANGED:{const auto bounds=reinterpret_cast<RECT*>(lparam);SetWindowPos(window,nullptr,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,SWP_NOZORDER|SWP_NOACTIVATE);app->Layout();return 0;}
        case WM_CTLCOLORSTATIC:SetTextColor(reinterpret_cast<HDC>(wparam),RGB(45,53,75));SetBkColor(reinterpret_cast<HDC>(wparam),RGB(246,248,253));return reinterpret_cast<LRESULT>(app->background);
        case WM_ERASEBKGND:{RECT bounds{};GetClientRect(window,&bounds);FillRect(reinterpret_cast<HDC>(wparam),&bounds,app->background);return 1;}
        case WM_CLOSE:if(app->tray_added)ShowWindow(window,SW_HIDE);else app->Exit();return 0;
        case WM_DESTROY:app->RemoveTray();app->loading=true;SetWindowTextW(app->key,L"");PostQuitMessage(0);return 0;
        case WM_NCDESTROY:SetWindowLongPtrW(window,GWLP_USERDATA,0);break;
        }
    }catch(...){if(message==WM_CREATE)return -1;}
    return DefWindowProcW(window,message,wparam,lparam);
}
HICON LoadBrand(const std::wstring& path){
    Gdiplus::Bitmap source(path.c_str());if(source.GetLastStatus()!=Gdiplus::Ok)return nullptr;
    Gdiplus::Bitmap scaled(32,32,PixelFormat32bppARGB);
    {Gdiplus::Graphics graphics(&scaled);graphics.Clear(Gdiplus::Color(0,0,0,0));graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);graphics.DrawImage(&source,0,0,32,32);}
    HICON icon{};scaled.GetHICON(&icon);return icon;
}
bool Preview(Application& app,const std::wstring& path){
    const auto window=app.window;
    RECT bounds{};GetWindowRect(window,&bounds);const auto width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
    HDC screen=GetDC(nullptr),memory=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,width,height);ReleaseDC(nullptr,screen);if(!memory||!bitmap)return false;
    const auto previous=SelectObject(memory,bitmap);SendMessageW(window,WM_PRINT,reinterpret_cast<WPARAM>(memory),PRF_CLIENT|PRF_NONCLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
    // WM_PRINT on the parent omits the Edit control's non-client frame on
    // Windows 10. Render that owned child explicitly, with no screen capture.
    for(const auto control:app.words_page?std::vector<HWND>{app.word_text,app.word_pinyin}:app.learning_page?std::vector<HWND>{}:std::vector<HWND>{app.key}){
        RECT field{};GetWindowRect(control,&field);const auto saved_context=SaveDC(memory);SetViewportOrgEx(memory,field.left-bounds.left,field.top-bounds.top,nullptr);
        SendMessageW(control,WM_PRINT,reinterpret_cast<WPARAM>(memory),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);RestoreDC(memory,saved_context);}SelectObject(memory,previous);
    bool saved{};
    {Gdiplus::Bitmap image(bitmap,nullptr);UINT count{},size{};Gdiplus::GetImageEncodersSize(&count,&size);std::vector<uint8_t> bytes(size);
    if(size&&Gdiplus::GetImageEncoders(count,size,reinterpret_cast<Gdiplus::ImageCodecInfo*>(bytes.data()))==Gdiplus::Ok){
        const auto encoders=reinterpret_cast<const Gdiplus::ImageCodecInfo*>(bytes.data());
        for(UINT i=0;i<count;++i)if(wcscmp(encoders[i].MimeType,L"image/png")==0){saved=image.Save(path.c_str(),&encoders[i].Clsid)==Gdiplus::Ok;break;}
    }}DeleteObject(bitmap);DeleteDC(memory);return saved;
}
#ifdef TI_SETTINGS_FIXTURE
void WordUiChecks(Application& app,testing::WordsFixture& fixture){
    const auto wait=[&]{const auto deadline=GetTickCount64()+5000;while(app.job&&GetTickCount64()<deadline){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}MsgWaitForMultipleObjectsEx(0,nullptr,5,QS_ALLINPUT,MWMO_INPUTAVAILABLE);}testing::WordsFixture::Need(!app.job,"word UI completion timeout");};
    app.SwitchPage(false,true);wait();testing::WordsFixture::Need(app.words_view.health==words_control::Ready&&app.words_view.total==0,"word UI initial status");
    SetWindowTextW(app.word_text,L"星河研究所");SetWindowTextW(app.word_pinyin,L"xing he yan jiu suo");
    SendMessageW(app.window,WM_COMMAND,WordAddId,0);wait();testing::WordsFixture::Need(app.words_view.total==1&&app.words_view.state==words_control::Succeeded,"word UI add");
    const auto candidate=QueryBroker(fixture.endpoint,fixture.executable,nullptr,80,1,1,"xingheyanjiusuo",15,fixture.pid);
    const std::string expected="星河研究所";testing::WordsFixture::Need(!candidate.status&&candidate.snapshot.candidate_count&&std::string(reinterpret_cast<const char*>(candidate.snapshot.candidates[0].bytes),candidate.snapshot.candidates[0].length)==expected,"word UI saved candidate");
    SetWindowTextW(app.word_text,L"云朵工作室");SetWindowTextW(app.word_pinyin,L"yun duo gong zuo shi");SendMessageW(app.window,WM_COMMAND,IDOK,0);wait();
    testing::WordsFixture::Need(app.words_view.total==2&&GetFileAttributesW(app.settings_path.c_str())==INVALID_FILE_ATTRIBUTES,"word-page Enter affected translation settings");
    SendMessageW(app.word_list,LB_SETCURSEL,0,0);SendMessageW(app.window,WM_COMMAND,MAKEWPARAM(WordListId,LBN_SELCHANGE),reinterpret_cast<LPARAM>(app.word_list));
    testing::WordsFixture::Need(IsWindowEnabled(app.word_remove)!=FALSE,"word UI selection did not enable delete");
    SendMessageW(app.window,WM_COMMAND,WordRemoveId,0);wait();testing::WordsFixture::Need(app.words_view.total==1&&app.words_view.state==words_control::Succeeded,"word UI delete");
    SetWindowTextW(app.word_text,L"ASCII");SetWindowTextW(app.word_pinyin,L"invalid3");SendMessageW(app.window,WM_COMMAND,WordAddId,0);wait();testing::WordsFixture::Need(app.words_view.total==1&&app.words_view.state==words_control::Failed,"word UI invalid input changed dictionary");
    app.SwitchPage(true);app.SwitchPage(false,true);wait();testing::WordsFixture::Need(app.words_view.total==1,"word UI refresh lost state");
    std::puts("PASS: owned hidden settings controls add/list/select/delete/validation/refresh and word-page Enter; fixed temporary broker, no real profile, key or network");
}
#endif
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
    int count{};LPWSTR* arguments=CommandLineToArgvW(GetCommandLineW(),&count);bool tray{},preview{},learning_preview{},words_preview{},words_check{};std::wstring output;
    if(arguments){for(int i=1;i<count;++i){const std::wstring argument=arguments[i];if(argument==L"--tray")tray=true;
#ifdef TI_SETTINGS_FIXTURE
        else if(argument==L"--self-check-words")words_check=true;
#endif
        else if((argument==L"--render-preview"||argument==L"--render-learning-preview"||argument==L"--render-words-preview")&&i+1<count){preview=true;learning_preview=argument==L"--render-learning-preview";words_preview=argument==L"--render-words-preview";output=arguments[++i];}else{LocalFree(arguments);return 2;}}LocalFree(arguments);}
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);Gdiplus::GdiplusStartupInput startup;ULONG_PTR gdiplus{};
    if(Gdiplus::GdiplusStartup(&gdiplus,&startup,nullptr)!=Gdiplus::Ok)return 3;
    int result{};
    try{
        const auto directory=pipe::ParentPath(pipe::ModulePath());Application app;app.fixture=preview||words_check;
        pipe::Handle singleton; if(!preview&&!words_check){pipe::Security security;const auto name=L"Local\\TransInput.Settings."+pipe::LogonSid();singleton.Reset(CreateMutexW(&security.attributes,FALSE,name.c_str()));
            if(!singleton)throw std::runtime_error("settings instance");if(GetLastError()==ERROR_ALREADY_EXISTS){if(const auto existing=FindWindowW(ClassName,nullptr))PostMessageW(existing,ShowSettings,0,0);Gdiplus::GdiplusShutdown(gdiplus);return 0;}}
        app.settings_path=preview||words_check?directory+L"\\preview-not-user-settings.bin":translation::SettingsPath();app.core_path=directory+L"\\transinput_core.dll";
        const auto shared=pipe::ParentPath(directory)+L"\\shared\\TransInputTranslator.exe";app.executable=GetFileAttributesW(shared.c_str())!=INVALID_FILE_ATTRIBUTES?shared:directory+L"\\TransInputTranslator.exe";
        const auto broker=pipe::ParentPath(directory)+L"\\shared\\TransInputBroker.exe";app.broker_executable=GetFileAttributesW(broker.c_str())!=INVALID_FILE_ATTRIBUTES?broker:directory+L"\\TransInputBroker.exe";
#ifdef TI_SETTINGS_FIXTURE
        std::unique_ptr<testing::WordsFixture> word_fixture;if(words_check){word_fixture=std::make_unique<testing::WordsFixture>();app.words_testing=true;app.words_endpoint=word_fixture->endpoint;app.broker_executable=word_fixture->executable;app.settings_path=word_fixture->root+L"\\never-settings.bin";}
#endif
        app.icon=LoadBrand(directory+L"\\transinput-icon.png");
        WNDCLASSEXW wc{sizeof wc};wc.hInstance=instance;wc.lpszClassName=ClassName;wc.lpfnWndProc=Procedure;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=app.icon;wc.hIconSm=app.icon;
        if(!RegisterClassExW(&wc))throw std::runtime_error("settings class");
        const auto dpi=GetDpiForSystem();RECT bounds{0,0,MulDiv(436,static_cast<int>(dpi),96),MulDiv(438,static_cast<int>(dpi),96)};
        AdjustWindowRectExForDpi(&bounds,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,0,dpi);
        const auto window=CreateWindowExW(0,ClassName,L"TransInput 设置",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,
            bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,instance,&app);if(!window)throw std::runtime_error("settings window");
        if(words_check){
#ifdef TI_SETTINGS_FIXTURE
            try{WordUiChecks(app,*word_fixture);}catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());result=1;}
            app.Exit();const auto deadline=GetTickCount64()+5000;while(app.job&&GetTickCount64()<deadline){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}MsgWaitForMultipleObjectsEx(0,nullptr,5,QS_ALLINPUT,MWMO_INPUTAVAILABLE);}if(app.job)ExitProcess(6);
#endif
        }
        else if(preview){if(learning_preview)app.SwitchPage(true);else if(words_preview)app.SwitchPage(false,true);result=Preview(app,output)?0:4;DestroyWindow(window);}
        else {app.TrayIcon();if(!tray||!app.tray_added)app.Show();MSG message{};while(GetMessageW(&message,nullptr,0,0)>0)if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
        UnregisterClassW(ClassName,instance);
    }catch(...){result=1;}
    Gdiplus::GdiplusShutdown(gdiplus);return result;
}
