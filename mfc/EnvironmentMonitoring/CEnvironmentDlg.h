#pragma once
#include "../Dialogs/HmiTheme.h"
#include "CEnvironmentGraph.h"
class CEnvironmentDlg final : public CHmiPage {
public:
    CEnvironmentDlg(CDataManager&, CCommandManager&, CWnd* parent=nullptr);
    CSize LayoutSize() const override {return CSize(1660,740);}
    int MinimumContentHeight() const override { return 780; }
    void Build() override;
    void Refresh() override;
protected:
    afx_msg void OnSize(UINT type,int cx,int cy);
    afx_msg void OnSaveEnvironment();
    afx_msg void OnExportSamples();
    afx_msg void OnExportAudit();
    DECLARE_MESSAGE_MAP()
private:
    enum : UINT { TempValue=2701, HumidityValue, StatusValue, StatusReason,
        ConnectionValue, BuzzerValue, LatestValue, CountValue, AlarmValue,
        TempGraph=2750, HumidityGraph=2751,
        AuditList=2752, WarnTempEdit=2753, WarnHumEdit=2754,
        CriticalTempEdit=2755, CriticalHumEdit=2756,
        SaveEnvironment=2757, ExportSamples=2758, ExportAudit=2759,
        PreviewNotice=2760 };
    CEnvironmentGraph m_temperatureGraph, m_humidityGraph;
    void PlaceGraphs();
    void ArrangeContent() override { PlaceGraphs(); }
    void RefreshAudit();
    size_t m_auditShown=static_cast<size_t>(-1);
    CString m_auditSignature;
};
