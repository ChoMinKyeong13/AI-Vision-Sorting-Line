#include "pch.h"
#include "CEnvironmentGraph.h"
#include <algorithm>
BEGIN_MESSAGE_MAP(CEnvironmentGraph,CStatic)
    ON_WM_PAINT()
END_MESSAGE_MAP()
void CEnvironmentGraph::OnPaint() {
    CPaintDC dc(this); CRect area; GetClientRect(&area);
    dc.FillSolidRect(area, RGB(255,255,255));
    if (area.Width()<140 || area.Height()<75) return;
    dc.SetBkMode(TRANSPARENT);
    const COLORREF accent=kind_==Kind::Temperature?RGB(191,65,74):RGB(41,110,167);
    const double ceiling=kind_==Kind::Temperature?50.0:100.0;
    CRect plot=area; plot.DeflateRect(48,17,16,31);
    CPen grid(PS_SOLID,1,RGB(229,235,243));
    auto oldPen=dc.SelectObject(&grid);
    dc.SetTextColor(RGB(110,128,148));
    for(int row=0;row<=4;++row){
        int y=plot.top+plot.Height()*row/4;
        dc.MoveTo(plot.left,y); dc.LineTo(plot.right,y);
        CString label;label.Format(L"%.0f",ceiling*(4-row)/4.0);
        dc.TextOutW(11,y-8,label);
    }
    dc.SelectObject(oldPen);
    if(!history_ || history_->Samples().empty()) {
        dc.SetTextColor(RGB(110,128,148));dc.DrawTextW(L"실시간 센서 데이터 수신 대기",plot,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        return;
    }
    const auto& data=history_->Samples();
    CPen line(PS_SOLID,3,accent);oldPen=dc.SelectObject(&line);
    bool hasPoint=false;unsigned long oldest=data.front().deviceMs, newest=data.back().deviceMs;
    for(const auto& sample:data){
        if(!sample.valid) {hasPoint=false;continue;}
        double value=kind_==Kind::Temperature?sample.temperature:sample.humidity;
        value=(std::max)(0.0,(std::min)(ceiling,value));
        int x=plot.left+(newest!=oldest?int((plot.Width()-1)*(sample.deviceMs-oldest)/double(newest-oldest)):plot.Width()/2);
        int y=plot.bottom-int(plot.Height()*value/ceiling);
        if(hasPoint)dc.LineTo(x,y);else dc.MoveTo(x,y);
        hasPoint=true;
    }
    dc.SelectObject(oldPen);
    dc.SetTextColor(RGB(110,128,148));
    CString caption;caption.Format(L"최근 측정 %u개  ·  센서 경과 시간 기반",static_cast<unsigned>(data.size()));
    dc.TextOutW(plot.left,plot.bottom+9,caption);
}
