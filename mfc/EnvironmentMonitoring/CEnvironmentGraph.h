#pragma once
#include <afxwin.h>
#include "CEnvironmentData.h"
class CEnvironmentGraph : public CStatic {
public:
    enum class Kind { Temperature, Humidity };
    void Configure(Kind kind, const Environment::History* history) { kind_=kind; history_=history; Invalidate(FALSE); }
protected:
    afx_msg void OnPaint();
    DECLARE_MESSAGE_MAP()
private:
    Kind kind_=Kind::Temperature;
    const Environment::History* history_=nullptr;
};
