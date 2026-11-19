#include "pch.h"
#include "CEnvironmentDlg.h"
#include "../resource.h"
#include "../Core/CDataManager.h"
#include "../Core/CCommandManager.h"
#include <algorithm>
#include <atlconv.h>
#include <cmath>
BEGIN_MESSAGE_MAP(CEnvironmentDlg,CHmiPage)
    ON_WM_SIZE()
    ON_BN_CLICKED(SaveEnvironment, &CEnvironmentDlg::OnSaveEnvironment)
    ON_BN_CLICKED(ExportSamples, &CEnvironmentDlg::OnExportSamples)
    ON_BN_CLICKED(ExportAudit, &CEnvironmentDlg::OnExportAudit)
END_MESSAGE_MAP()
CEnvironmentDlg::CEnvironmentDlg(CDataManager& data,CCommandManager& commands,CWnd* parent)
    :CHmiPage(IDD_ENVIRONMENT_DLG,data,commands,parent) {}
void CEnvironmentDlg::Build() {
    Panel({0,0,532,164},L"TEMPERATURE  ·  실시간 온도",L"DHT11 / °C");
    Panel({546,0,1078,164},L"HUMIDITY  ·  실시간 습도",L"DHT11 / %RH");
    Panel({1092,0,1660,164},L"ENVIRONMENT HEALTH",L"MCU 판정 · 센서 상태");
    Text(TempValue,L"--.- °C",{23,60,500,126},28,Hmi::Ink,true).ModifyStyle(SS_TYPEMASK, SS_CENTER | SS_CENTERIMAGE);
    Text(HumidityValue,L"--.- %",{568,60,1050,126},28,Hmi::Ink,true).ModifyStyle(SS_TYPEMASK, SS_CENTER | SS_CENTERIMAGE);
    Text(StatusValue,L"데이터 수신 대기",{1115,51,1624,91},18,Hmi::Red,true).ModifyStyle(SS_TYPEMASK, SS_CENTER | SS_CENTERIMAGE | SS_ENDELLIPSIS);
    Text(StatusReason,L"센서 데이터를 기다리는 중",{1115,104,1631,148},9,Hmi::Muted).ModifyStyle(SS_TYPEMASK, SS_CENTER | SS_CENTERIMAGE | SS_ENDELLIPSIS);
    Panel({0,177,820,489},L"TEMPERATURE TREND",L"최근 실제 측정값 300개");
    Panel({834,177,1660,489},L"HUMIDITY TREND",L"최근 실제 측정값 300개");
    m_temperatureGraph.Create(L"",WS_CHILD|WS_VISIBLE|SS_NOTIFY,{22,226,797,468},this,TempGraph);
    m_temperatureGraph.Configure(CEnvironmentGraph::Kind::Temperature,&m_data.environmentHistory);
    m_humidityGraph.Create(L"",WS_CHILD|WS_VISIBLE|SS_NOTIFY,{856,226,1636,468},this,HumidityGraph);
    m_humidityGraph.Configure(CEnvironmentGraph::Kind::Humidity,&m_data.environmentHistory);
    Panel({0,501,820,740},L"환경 경고 기준",L"운전 상태와 무관 · 저장 응답 확인 후 적용");
    Panel({834,501,1660,740},L"EVENT AUDIT TRAIL",L"상태 변화 · 설정 변경 · 최대 500건");
    Text(ConnectionValue,L"ENV 수신 대기",{18,543,801,577},10,Hmi::Red,true).ModifyStyle(SS_TYPEMASK, SS_CENTER | SS_CENTERIMAGE | SS_ENDELLIPSIS);
    Text(LatestValue,L"마지막 수신 없음",{18,582,801,608},9,Hmi::Muted).ModifyStyle(SS_TYPEMASK, SS_CENTER | SS_CENTERIMAGE | SS_ENDELLIPSIS);
    Text(2790,L"주의 °C",{20,616,104,652},9,Hmi::Ink);
    Text(2791,L"주의 %",{215,616,292,652},9,Hmi::Ink);
    Text(2792,L"위험 °C",{410,616,487,652},9,Hmi::Ink);
    Text(2793,L"위험 %",{605,616,682,652},9,Hmi::Ink);
    auto setup=[&](UINT id,CRect r,double v){ auto& e=Edit(id,r,false,true); e.ModifyStyle(0,ES_CENTER); CString t;t.Format(L"%.1f",v);e.SetWindowText(t);};
    setup(WarnTempEdit,{106,616,190,652},m_data.environmentPreviewWarnTemp);
    setup(WarnHumEdit,{294,616,385,652},m_data.environmentPreviewWarnHumidity);
    setup(CriticalTempEdit,{489,616,580,652},m_data.environmentPreviewCriticalTemp);
    setup(CriticalHumEdit,{684,616,795,652},m_data.environmentPreviewCriticalHumidity);
    Button(SaveEnvironment,L"환경 기준 저장",{20,662,418,700},Hmi::Tone::Blue);
    Button(ExportSamples,L"측정 CSV",{430,662,600,700});
    Button(ExportAudit,L"이벤트 CSV",{612,662,797,700});
    Text(PreviewNotice,L"입력값은 저장 후 MCU 일치 응답으로 확인 · 재부팅 시 기본값 복귀",{20,707,797,737},9,Hmi::Muted);
    List(AuditList,{856,552,1638,711},{{L"PC 시각",178},{L"유형",146},{L"이벤트 내용",435}});
    Text(AlarmValue,L"아직 이벤트가 없습니다.",{858,712,1630,735},9,Hmi::Muted);
    Refresh();
}
void CEnvironmentDlg::OnSize(UINT type,int cx,int cy){CHmiPage::OnSize(type,cx,cy);PlaceGraphs();}
void CEnvironmentDlg::PlaceGraphs(){
    if(!m_temperatureGraph.GetSafeHwnd()||!m_humidityGraph.GetSafeHwnd())return;
    CRect r;GetClientRect(r);if(r.Width()<1||r.Height()<1)return;
    auto place=[&](CEnvironmentGraph& graph,int l,int t,int right,int bottom){
        graph.MoveWindow(MapLayoutRect({l,t,right,bottom}),FALSE);
    };
    place(m_temperatureGraph,22,226,797,470);place(m_humidityGraph,856,226,1636,470);
}
void CEnvironmentDlg::Refresh(){
    const bool canSave=m_commands.CanApplyEnvironment();
    Enable(SaveEnvironment,canSave);
    const auto blocked=m_data.EnvironmentSettingsBlockedReason();
    SetText(PreviewNotice,!canSave&&!m_data.environmentAwaitingAck&&m_data.environmentPendingCommand.IsEmpty()?
        (blocked.IsEmpty()?CString(L"다른 명령·준비·복구 응답 대기 중 · 완료 후 저장 가능"):blocked):m_data.environmentApplyNotice);
    SetText(LatestValue,m_data.EnvironmentAppliedThresholdsText());
    const auto& history=m_data.environmentHistory.Samples();
    if(history.empty()){
        SetText(TempValue,L"--.- °C");SetText(HumidityValue,L"--.- %");
        SetText(StatusValue,L"연결 대기");SetTone(StatusValue,Hmi::Tone::Red);
        SetText(StatusReason,m_data.environmentProtocol?L"환경 모듈 부팅 확인 · 첫 실제 측정 대기":L"DHT11 ENV 미수신 · COM 연결과 지원 펌웨어·센서 확인 필요");
        SetText(ConnectionValue,L"ENV 데이터 미수신");SetTone(ConnectionValue,Hmi::Tone::Red);
        RefreshAudit();return;
    }
    const auto& sample=history.back();
    const bool fresh=m_data.environmentLastReceivedTick!=0 &&
        GetTickCount64()-m_data.environmentLastReceivedTick<=6500;
    CString text;
    if(sample.valid){text.Format(L"%.1f °C",sample.temperature);SetText(TempValue,text);
        text.Format(L"%.1f %%",sample.humidity);SetText(HumidityValue,text);}
    else {SetText(TempValue,L"측정 실패");SetText(HumidityValue,L"측정 실패");}
    if(!fresh){SetText(StatusValue,L"수신 끊김");SetTone(StatusValue,Hmi::Tone::Red);
        SetText(StatusReason,L"최근 6.5초 내 새 데이터가 없습니다. USB·센서를 점검하세요.");}
    else {
        switch(sample.state){
        case Environment::State::Normal:SetText(StatusValue,L"NORMAL  ·  정상");SetTone(StatusValue,Hmi::Tone::Green);break;
        case Environment::State::Warning:SetText(StatusValue,L"WARNING  ·  주의");SetTone(StatusValue,Hmi::Tone::Amber);break;
        case Environment::State::Critical:SetText(StatusValue,L"CRITICAL  ·  위험");SetTone(StatusValue,Hmi::Tone::Red);break;
        default:SetText(StatusValue,L"SENSOR ERROR  ·  센서 이상");SetTone(StatusValue,Hmi::Tone::Red);break;
        }
        SetText(StatusReason,L"DHT11 기준 상태 판정 · 시연용 경고 설정, 산업 안전 인증값 아님");
    }
    text.Format(fresh?L"● 환경 데이터 수신 중 · #%lu · %u개 기록":L"● 최신 데이터 확인 필요 · 마지막 #%lu · %u개 기록",
        sample.sequence,static_cast<unsigned>(history.size()));SetText(ConnectionValue,text);
    SetTone(ConnectionValue,fresh?Hmi::Tone::Green:Hmi::Tone::Red);
    RefreshAudit();
    if(m_temperatureGraph.GetSafeHwnd())m_temperatureGraph.Invalidate(FALSE);
    if(m_humidityGraph.GetSafeHwnd())m_humidityGraph.Invalidate(FALSE);
}

namespace {
bool ReadThreshold(CWnd* control,double& result) {
    if(!control)return false;
    CString raw;control->GetWindowText(raw);raw.Trim();
    if(raw.IsEmpty())return false;
    wchar_t* end=nullptr;
    double value=wcstod(raw.GetString(),&end);
    if(end==raw.GetString() || !end || *end!=L'\0' || !std::isfinite(value))return false;
    result=value;return true;
}
CString CsvCell(const CString& value) {
    CString escaped(value); escaped.Replace(L"\"",L"\"\"");
    return L"\""+escaped+L"\"";
}
bool WriteCsvUtf8(const CString& fileName,const CString& csv) {
    HANDLE handle=CreateFileW(fileName,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;
    const BYTE bom[]={0xEF,0xBB,0xBF};DWORD written=0;
    bool ok=WriteFile(handle,bom,sizeof(bom),&written,nullptr)!=FALSE && written==sizeof(bom);
    CStringA utf8(CW2A(csv.GetString(),CP_UTF8));
    const DWORD bytes=static_cast<DWORD>(utf8.GetLength());
    if(ok)ok=WriteFile(handle,utf8.GetString(),bytes,&written,nullptr)!=FALSE && written==bytes;
    CloseHandle(handle);return ok;
}
CString StateName(Environment::State s) {
    switch(s){case Environment::State::Normal:return L"NORMAL";
    case Environment::State::Warning:return L"WARNING";
    case Environment::State::Critical:return L"CRITICAL";
    default:return L"SENSOR_ERROR";}
}
}
void CEnvironmentDlg::RefreshAudit() {
    auto* list=static_cast<CListCtrl*>(Control(AuditList));
    if(!list)return;
    const auto& entries=m_data.environmentAudit;
    CString signature;
    if(!entries.empty())signature=entries.front().time+entries.front().category+entries.front().message;
    if(m_auditShown==entries.size() && signature==m_auditSignature)return;
    m_auditShown=entries.size();m_auditSignature=signature;
    list->SetRedraw(FALSE);list->DeleteAllItems();
    int row=0;
    for(const auto& e:entries){
        int index=list->InsertItem(row++,e.time);
        list->SetItemText(index,1,e.category);
        list->SetItemText(index,2,e.message);
    }
    list->SetRedraw(TRUE);list->Invalidate(FALSE);
    CString count;count.Format(L"현재 세션 이벤트 %u건 · CSV로 내보내기 가능",
        static_cast<unsigned>(entries.size()));
    SetText(AlarmValue,count);
}
void CEnvironmentDlg::OnSaveEnvironment(){
    double wt=0,wh=0,ct=0,ch=0;
    if(!ReadThreshold(Control(WarnTempEdit),wt)||!ReadThreshold(Control(WarnHumEdit),wh)||
        !ReadThreshold(Control(CriticalTempEdit),ct)||!ReadThreshold(Control(CriticalHumEdit),ch)||
        wt<0||wt>=ct||ct>60||wh<0||wh>=ch||ch>100){
        AfxMessageBox(L"온도 0~60°C·습도 0~100%에서 주의 < 위험 값을 입력하세요.",MB_ICONWARNING);return;
    }
    if(!m_commands.CanApplyEnvironment()){
        const auto reason=m_data.EnvironmentSettingsBlockedReason();
        AfxMessageBox(reason.IsEmpty()?CString(L"다른 명령·준비·복구 응답 대기 중입니다."):reason,MB_ICONWARNING);return;
    }
    if(!m_commands.ApplyEnvironmentThresholds(wt,wh,ct,ch))
        AfxMessageBox(L"환경 기준을 저장하지 못했습니다. 소수점 한 자리 변환 후 주의 < 위험과 USB·환경 수신 상태를 확인하세요.",MB_ICONWARNING);
    Refresh();
}
void CEnvironmentDlg::OnExportSamples(){
    const auto& samples=m_data.environmentHistory.Samples();
    if(samples.empty()){AfxMessageBox(L"저장할 측정 데이터가 없습니다.");return;}
    CFileDialog dlg(FALSE,L"csv",L"environment_samples.csv",OFN_OVERWRITEPROMPT|OFN_HIDEREADONLY,
        L"CSV 파일 (*.csv)|*.csv||",this);
    if(dlg.DoModal()!=IDOK)return;
    CString csv=L"sequence,device_ms,temperature_c,humidity_pct,state,muted,valid\r\n";
    for(const auto& s:samples){CString row;row.Format(L"%lu,%lu,%.2f,%.2f,%s,%d,%d\r\n",
        s.sequence,s.deviceMs,s.temperature,s.humidity,StateName(s.state).GetString(),s.muted?1:0,s.valid?1:0);csv+=row;}
    if(!WriteCsvUtf8(dlg.GetPathName(),csv))AfxMessageBox(L"측정 CSV 파일 저장 실패",MB_ICONERROR);
    else {m_data.AddEnvironmentAudit(L"CSV 저장",L"측정 이력 CSV 내보내기");RefreshAudit();AfxMessageBox(L"측정 CSV 저장 완료");}
}
void CEnvironmentDlg::OnExportAudit(){
    if(m_data.environmentAudit.empty()){AfxMessageBox(L"저장할 이벤트가 없습니다.");return;}
    CFileDialog dlg(FALSE,L"csv",L"environment_events.csv",OFN_OVERWRITEPROMPT|OFN_HIDEREADONLY,
        L"CSV 파일 (*.csv)|*.csv||",this);
    if(dlg.DoModal()!=IDOK)return;
    CString csv=L"pc_time,category,event,temperature_c,humidity_pct\r\n";
    for(const auto& e:m_data.environmentAudit){
        csv+=CsvCell(e.time)+L","+CsvCell(e.category)+L","+CsvCell(e.message)+L",";
        if(e.hasMeasurement){CString values;values.Format(L"%.2f,%.2f",e.temperature,e.humidity);csv+=values;}
        else csv+=L",";
        csv+=L"\r\n";
    }
    if(!WriteCsvUtf8(dlg.GetPathName(),csv))AfxMessageBox(L"이벤트 CSV 파일 저장 실패",MB_ICONERROR);
    else {m_data.AddEnvironmentAudit(L"CSV 저장",L"이벤트 CSV 내보내기");RefreshAudit();AfxMessageBox(L"이벤트 CSV 저장 완료");}
}
