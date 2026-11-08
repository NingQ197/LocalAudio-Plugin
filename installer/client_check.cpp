// Read-only process check. Never tests Program Files write permissions.
#include <windows.h>
#include <tlhelp32.h>
#include <string>
std::wstring normal(const wchar_t* p){
 wchar_t full[32768]={},longName[32768]={};
 if(!GetFullPathNameW(p,32768,full,nullptr))return {};
 const wchar_t* result=GetLongPathNameW(full,longName,32768)?longName:full;
 std::wstring s=result;
 HANDLE h=CreateFileW(result,0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(h!=INVALID_HANDLE_VALUE){wchar_t resolved[32768]={};DWORD n=GetFinalPathNameByHandleW(h,resolved,32768,FILE_NAME_NORMALIZED);if(n&&n<32768)s=resolved;CloseHandle(h);}
 if(s.rfind(L"\\\\?\\",0)==0)s.erase(0,4);return s;
}
int wmain(int argc,wchar_t** argv){
 if(argc!=2)return 3;const auto target=normal(argv[1]);if(target.empty())return 3;
 const auto pos=target.find_last_of(L"\\/");const auto name=target.substr(pos==std::wstring::npos?0:pos+1);
 HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE)return 2;
 PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);int result=0;
 if(!Process32FirstW(snapshot,&entry)){CloseHandle(snapshot);return 2;}
 do{
  if(_wcsicmp(entry.szExeFile,name.c_str())!=0)continue;
  HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID);
  if(!process){if(GetLastError()!=ERROR_INVALID_PARAMETER)result=2;continue;}
  wchar_t path[32768]={};DWORD size=32768;
  if(QueryFullProcessImageNameW(process,0,path,&size)){
   const auto found=normal(path);if(_wcsicmp(found.c_str(),target.c_str())==0){CloseHandle(process);result=1;break;}
  }else{DWORD status=0;if(!GetExitCodeProcess(process,&status)||status==STILL_ACTIVE)result=2;}
  CloseHandle(process);
 }while(Process32NextW(snapshot,&entry));
 CloseHandle(snapshot);return result;
}
