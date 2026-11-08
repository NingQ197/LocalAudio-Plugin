// A C-ABI bridge built against the exact Qt 5.15.2 MSVC ABI shipped in TS3 3.6.2.
// No TeamSpeak executable patches and no separate background application.
#include "denoise_ui_api.h"
#include <QtWidgets/QApplication>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QStyle>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTextStream>
#include <QtCore/QThread>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtCore/QSettings>
#include <QtCore/QSaveFile>
#include <QtCore/QFileInfo>
#include <QtCore/QElapsedTimer>
#include <QtGui/QCloseEvent>
#include <cstring>
#include <algorithm>
#include <vector>

namespace {
NingqNoiseUiCallbacks callbacks{};
using OpenAudioCallback=void(*)();
class GuideDialog final:public QDialog {
    QString statePath;
    OpenAudioCallback openAudio;
    QStackedWidget* pages;
    QPushButton *firstStep,*secondStep,*back,*next,*open,*skip;
    QLabel* error;
    bool saveFailed=false;
    bool persist(){
        QSaveFile file(statePath);
        const QByteArray content("[Tutorial]\ncompletedVersion=1\n");
        if(file.open(QIODevice::WriteOnly)&&file.write(content)==content.size()&&file.commit())return true;
        error->setText(QString::fromUtf8("无法保存引导状态，下次启动还会显示。再次点击关闭可退出。"));
        error->show();saveFailed=true;return false;
    }
    void finish(bool launch){
        if(!saveFailed&&!persist())return;
        const auto callback=openAudio;QDialog::done(QDialog::Accepted);
        if(launch&&callback)callback();
    }
    QWidget* page(const QString& heading,const QString& text,const QString& hint){
        auto* result=new QWidget(this);auto* layout=new QVBoxLayout(result);layout->setContentsMargins(0,0,0,0);layout->setSpacing(13);
        auto* title=new QLabel(heading,result);title->setObjectName("ningqGuideHeading");title->setWordWrap(true);layout->addWidget(title);
        auto* instructions=new QLabel(text,result);instructions->setObjectName("ningqGuideInstructions");instructions->setTextFormat(Qt::RichText);instructions->setWordWrap(true);instructions->setOpenExternalLinks(false);layout->addWidget(instructions);
        auto* note=new QLabel(hint,result);note->setObjectName("ningqGuideHint");note->setWordWrap(true);layout->addWidget(note);layout->addStretch();return result;
    }
public:
    GuideDialog(QWidget* parent,const QString& path,OpenAudioCallback callback):QDialog(parent),statePath(path),openAudio(callback){
        setObjectName("ningqFirstRunGuide");setWindowTitle(QString::fromUtf8("LocalAudio 使用引导"));
        setAttribute(Qt::WA_DeleteOnClose);setWindowModality(Qt::NonModal);setMinimumWidth(620);resize(700,390);
        setStyleSheet("QDialog#ningqFirstRunGuide{background:#ffffff;color:#173e40;font-family:'Microsoft YaHei UI';font-size:13px;}"
                      "QDialog#ningqFirstRunGuide QLabel,QDialog#ningqFirstRunGuide QPushButton{font-family:'Microsoft YaHei UI';font-size:13px;}"
                      "QDialog#ningqFirstRunGuide QLabel{color:#173e40;} QDialog#ningqFirstRunGuide QLabel#ningqGuideHeading{font-size:24px;font-weight:600;}"
                      "QDialog#ningqFirstRunGuide QLabel#ningqGuideInstructions{font-size:14px;line-height:1.5;} QLabel#ningqGuideHint{background:#eef9f8;color:#356569;padding:12px;}"
                      "QPushButton{min-height:34px;padding:0 14px;border:1px solid #b6d1d0;border-radius:5px;background:#ffffff;color:#285d60;}"
                      "QPushButton:hover{background:#eef9f8;} QPushButton:focus{border:2px solid #147f82;} QPushButton:disabled{color:#90a7a8;border-color:#d8e4e4;}"
                      "QPushButton#ningqGuideNext,QPushButton#ningqGuideOpen{background:#147f82;color:#ffffff;border-color:#147f82;}"
                      "QPushButton[activeStep=\"true\"]{background:#eef9f8;color:#0d696b;border-color:#147f82;font-weight:600;}"
                      "QDialog#ningqFirstRunGuide QLabel#ningqGuideBrand{font-size:11px;color:#6a8c8e;} QLabel#ningqGuideError{color:#a23d32;}");
        auto* root=new QVBoxLayout(this);root->setContentsMargins(0,0,0,0);root->setSpacing(0);
        auto* body=new QWidget(this);auto* layout=new QVBoxLayout(body);layout->setContentsMargins(26,18,26,20);layout->setSpacing(17);root->addWidget(body,1);
        auto* steps=new QHBoxLayout;firstStep=new QPushButton(QString::fromUtf8("1  AI 降噪"),this);secondStep=new QPushButton(QString::fromUtf8("2  音频共享"),this);
        firstStep->setObjectName("ningqGuideStep1");secondStep->setObjectName("ningqGuideStep2");steps->addWidget(firstStep);steps->addWidget(secondStep);steps->addStretch();
        auto* brand=new QLabel("Design by ningq",this);brand->setObjectName("ningqGuideBrand");steps->addWidget(brand);layout->addLayout(steps);
        pages=new QStackedWidget(this);pages->setObjectName("ningqGuidePages");
        pages->addWidget(page(QString::fromUtf8("先让麦克风声音更清楚"),
          QString::fromUtf8("打开 <b>工具 → 选项 → 音频输入</b>，找到“AI 麦克风降噪”。<br><br>选择 <b>GTCRN</b> 或 <b>RNNoise</b>，再调整“降噪强度”，点击“应用”保存。<br><br>建议从 <b>70%</b> 开始，试听说话效果；人声变闷或被削弱时，适当降低强度。"),
          QString::fromUtf8("只处理你的麦克风，分享的音乐保持原样。建议关闭 TS 自带“消除背景噪音”，避免重复降噪。")));
        pages->addWidget(page(QString::fromUtf8("把正在播放的音乐分享给频道"),
          QString::fromUtf8("从 TS 的 <b>插件菜单</b> 打开 <b>LocalAudio</b>。<br><br>选择 <b>QQ 音乐、网易云音乐、Edge 或 Chrome</b>；找不到目标程序时点击“扫描应用”，再选择带图标的应用。<br><br>在该应用中播放音乐，然后点击 <b>“开始声音转播”</b>。频道成员即可收听，结束时点击“停止转播”。"),
          QString::fromUtf8("“音乐播放音量”调整别人听到的音乐大小。完成本教程不会开启麦克风采集或音乐转播。")));
        layout->addWidget(pages,1);error=new QLabel(this);error->setObjectName("ningqGuideError");error->setWordWrap(true);error->hide();layout->addWidget(error);
        auto* controls=new QHBoxLayout;skip=new QPushButton(QString::fromUtf8("暂不设置"),this);skip->setObjectName("ningqGuideSkip");controls->addWidget(skip);controls->addStretch();
        back=new QPushButton(QString::fromUtf8("上一步"),this);back->setObjectName("ningqGuideBack");controls->addWidget(back);
        open=new QPushButton(QString::fromUtf8("打开音频共享"),this);open->setObjectName("ningqGuideOpen");controls->addWidget(open);
        next=new QPushButton(this);next->setObjectName("ningqGuideNext");controls->addWidget(next);layout->addLayout(controls);
        connect(firstStep,&QPushButton::clicked,this,[this]{select(0);});connect(secondStep,&QPushButton::clicked,this,[this]{select(1);});
        connect(back,&QPushButton::clicked,this,[this]{select(0);});connect(next,&QPushButton::clicked,this,[this]{if(pages->currentIndex()==0)select(1);else finish(false);});
        connect(skip,&QPushButton::clicked,this,[this]{finish(false);});connect(open,&QPushButton::clicked,this,[this]{finish(true);});select(0);
    }
    void select(int step){
        pages->setCurrentIndex(step);firstStep->setProperty("activeStep",step==0);secondStep->setProperty("activeStep",step==1);
        for(auto* button:{firstStep,secondStep}){button->style()->unpolish(button);button->style()->polish(button);}
        back->setVisible(step==1);open->setVisible(step==1&&openAudio);next->setText(QString::fromUtf8(step==0?"下一步":"完成"));next->setDefault(true);
    }
    void reject()override{if(!saveFailed&&!persist())return;QDialog::reject();}
    void closeEvent(QCloseEvent* event)override{
        if(!saveFailed&&!persist()){event->ignore();return;}QDialog::closeEvent(event);
    }
};
class TutorialController final:public QObject {
    QString statePath;
    OpenAudioCallback openAudio;
    QPointer<GuideDialog> dialog;
    QElapsedTimer ready;
    bool automaticPending=true;
    QWidget* mainWindow(){
        for(auto* window:QApplication::topLevelWidgets())
            if(qobject_cast<QMainWindow*>(window)&&window->isVisible()&&!window->isMinimized())return window;
        return nullptr;
    }
public:
    TutorialController(const QString& profile,OpenAudioCallback callback):statePath(QDir(profile).filePath("localaudio-guide.ini")),openAudio(callback){
        QSettings stored(statePath,QSettings::IniFormat);automaticPending=stored.value("Tutorial/completedVersion",0).toInt()<1;
        auto* timer=new QTimer(this);connect(timer,&QTimer::timeout,this,[this]{tick();});timer->start(350);
    }
    ~TutorialController(){if(dialog)delete dialog;}
    void showGuide(){
        if(dialog){dialog->show();dialog->raise();dialog->activateWindow();return;}
        automaticPending=false;dialog=new GuideDialog(mainWindow(),statePath,openAudio);dialog->show();
    }
    void tick(){
        if(!automaticPending)return;
        bool blocked=QApplication::activeModalWidget()!=nullptr;
        for(auto* window:QApplication::topLevelWidgets())if(qobject_cast<QDialog*>(window)&&window->isVisible())blocked=true;
        if(blocked||!mainWindow()){ready.invalidate();return;}
        if(!ready.isValid()){ready.start();return;}if(ready.elapsed()>=700)showGuide();
    }
};
TutorialController* tutorial=nullptr;
bool containsAnchor(QLayout* layout,QWidget* anchor){
    if(layout->indexOf(anchor)>=0)return true;
    for(int i=0;i<layout->count();++i)if(auto* nested=layout->itemAt(i)->layout())if(containsAnchor(nested,anchor))return true;
    return false;
}
// Locate the innermost vertical container that actually owns the native noise
// control. Never append to the capture page's horizontal profile/content split.
QLayout* insertionLayout(QLayout* layout,QWidget* anchor){
    if(!layout)return nullptr;
    for(int i=0;i<layout->count();++i){
        auto* item=layout->itemAt(i);
        if(auto* nested=item->layout())if(auto* target=insertionLayout(nested,anchor))return target;
    }
    if(!containsAnchor(layout,anchor))return nullptr;
    if(auto* box=qobject_cast<QBoxLayout*>(layout)){
        if(box->direction()==QBoxLayout::TopToBottom)return box;
    }
    if(qobject_cast<QGridLayout*>(layout)||qobject_cast<QFormLayout*>(layout))return layout;
    return nullptr;
}
class Panel final:public QGroupBox {
    QRadioButton *off,*gt,*rn;
    QSlider* strength;
    QLabel *value,*status;
    QPointer<QDialog> dialog;
    int savedModel=0,savedStrength=70;
    bool dirty=false;
public:
    Panel(QWidget* page,QDialog* options):QGroupBox(QString::fromUtf8("AI 麦克风降噪"),page),dialog(options){
        setObjectName("ningqAiDenoiseGroup");
        auto* layout=new QVBoxLayout(this);layout->setContentsMargins(12,10,12,10);layout->setSpacing(8);
        auto* models=new QHBoxLayout;
        off=new QRadioButton(QString::fromUtf8("关闭"),this);off->setObjectName("ningqAiModelOff");
        gt=new QRadioButton("GTCRN",this);gt->setObjectName("ningqAiModelGTCRN");
        rn=new QRadioButton("RNNoise",this);rn->setObjectName("ningqAiModelRNNoise");
        gt->setToolTip(QString::fromUtf8("GTCRN 使用 16 kHz 人声模型，完整输出会减少高频成分。"));
        rn->setToolTip(QString::fromUtf8("RNNoise 使用 48 kHz 实时降噪模型。"));
        models->addWidget(off);models->addWidget(gt);models->addWidget(rn);models->addStretch();layout->addLayout(models);
        auto* row=new QHBoxLayout;
        value=new QLabel(this);value->setMinimumWidth(112);row->addWidget(value);
        row->addWidget(new QLabel(QString::fromUtf8("弱"),this));
        strength=new QSlider(Qt::Horizontal,this);strength->setObjectName("ningqAiStrength");strength->setRange(0,100);strength->setSingleStep(5);strength->setPageStep(10);strength->setTickInterval(20);strength->setAccessibleName(QString::fromUtf8("AI 降噪强度"));row->addWidget(strength,1);
        strength->setToolTip(QString::fromUtf8("0% 保留原始麦克风声音；100% 完全采用降噪模型输出。"));
        row->addWidget(new QLabel(QString::fromUtf8("强"),this));layout->addLayout(row);
        status=new QLabel(this);status->setObjectName("ningqAiStatus");status->setWordWrap(true);layout->addWidget(status);
        auto* hint=new QLabel(QString::fromUtf8("建议关闭上方“消除背景噪音”，避免重复降噪。"),this);hint->setWordWrap(true);layout->addWidget(hint);
        auto* brand=new QLabel("Design by ningq",this);brand->setAlignment(Qt::AlignRight);brand->setStyleSheet("color: #777; font-size: 11px;");layout->addWidget(brand);
        callbacks.read(&savedModel,&savedStrength);
        (savedModel==1?gt:savedModel==2?rn:off)->setChecked(true);strength->setValue(savedStrength);
        for(auto* b:{off,gt,rn})connect(b,&QRadioButton::toggled,this,[this](bool selected){if(selected)changed();});
        connect(strength,&QSlider::valueChanged,this,[this]{changed();});
        connect(options,&QDialog::accepted,this,[this]{commit();});
        connect(options,&QDialog::rejected,this,[this]{reload();});
        for(auto* box:options->findChildren<QDialogButtonBox*>())connect(box,&QDialogButtonBox::clicked,this,[this,box](QAbstractButton* b){if(box->buttonRole(b)==QDialogButtonBox::ApplyRole)commit();});
        for(auto* button:options->findChildren<QPushButton*>()){
            auto label=button->text();label.remove('&');
            if(label.compare("Apply",Qt::CaseInsensitive)==0||label.contains(QString::fromUtf8("应用")))connect(button,&QPushButton::clicked,this,[this]{commit();});
        }
        refresh();
    }
    int model()const{return gt->isChecked()?1:rn->isChecked()?2:0;}
    void reload(){
        const QSignalBlocker a(off),b(gt),c(rn),d(strength);
        callbacks.read(&savedModel,&savedStrength);
        (savedModel==1?gt:savedModel==2?rn:off)->setChecked(true);strength->setValue(savedStrength);dirty=false;refresh();
    }
    void changed(){
        dirty=model()!=savedModel||strength->value()!=savedStrength;
        if(dirty&&dialog)for(auto* box:dialog->findChildren<QDialogButtonBox*>())if(auto* b=box->button(QDialogButtonBox::Apply))b->setEnabled(true);
        if(dirty&&dialog)for(auto* button:dialog->findChildren<QPushButton*>()){auto label=button->text();label.remove('&');if(label.compare("Apply",Qt::CaseInsensitive)==0||label.contains(QString::fromUtf8("应用")))button->setEnabled(true);}
        refresh();
    }
    void refresh(){
        if(!dirty){int m=0,s=70;callbacks.read(&m,&s);if(m!=savedModel||s!=savedStrength){reload();return;}}
        value->setText(QString::fromUtf8("降噪强度：%1%").arg(strength->value()));strength->setEnabled(model()!=0);
        if(dirty)status->setText(QString::fromUtf8("尚未应用，点击“应用”或“确定”保存。"));
        else {wchar_t text[256]={};callbacks.status(text,256);status->setText(QString::fromWCharArray(text));}
    }
    void commit(){
        if(!dirty)return;
        if(!callbacks.write(model(),strength->value())){QMessageBox::warning(dialog,QString::fromUtf8("AI 降噪"),QString::fromUtf8("保存降噪设置失败，原设置已保留。"));return;}
        savedModel=model();savedStrength=strength->value();dirty=false;refresh();
    }
};
class Controller final:public QObject {
    QSet<QWidget*> attached;
    std::vector<QPointer<Panel>> panels;
    QSet<QDialog*> dumped;
    void dump(QDialog* dialog){
        if(!qEnvironmentVariableIsSet("LOCAL_AUDIO_UI_TREE")||dumped.contains(dialog))return;
        const auto name=QDir::tempPath()+QString("/ningq-denoise-ui-%1.txt").arg(QCoreApplication::applicationPid());
        dumped.insert(dialog);QFile f(name);if(!f.open(QIODevice::WriteOnly|QIODevice::Text|QIODevice::NewOnly))return;QTextStream out(&f);out.setCodec("UTF-8");
        for(auto* w:dialog->findChildren<QWidget*>()){int depth=0;for(auto* p=w->parentWidget();p&&p!=dialog;p=p->parentWidget())++depth;QString text;if(auto* label=qobject_cast<QLabel*>(w))text=label->text();if(auto* b=qobject_cast<QAbstractButton*>(w))text=b->text();out<<QString(depth*2,' ')<<w->metaObject()->className()<<" "<<w->objectName()<<" "<<text<<"\n";}
    }
public:
    Controller(){auto* timer=new QTimer(this);connect(timer,&QTimer::timeout,this,[this]{scan();});timer->start(350);scan();}
    ~Controller(){for(auto panel:panels)if(panel)delete panel;}
    void scan(){
        for(auto panel:panels)if(panel&&panel->isVisible())panel->refresh();
        for(auto* w:QApplication::allWidgets()){
            auto* check=qobject_cast<QCheckBox*>(w);if(!check||!check->isVisible())continue;
            const auto text=check->text();
            if(!text.contains(QString::fromUtf8("消除背景噪"))&&!text.contains("Remove background noise",Qt::CaseInsensitive))continue;
            auto* dialog=qobject_cast<QDialog*>(check->window());if(!dialog)continue;dump(dialog);
            QWidget* page=check->parentWidget();QLayout* target=nullptr;QWidget* anchor=check;
            while(page&&page!=dialog){
                target=insertionLayout(page->layout(),anchor);if(target)break;
                anchor=page;page=page->parentWidget();
                if(qobject_cast<QStackedWidget*>(page))break;
            }
            if(!target||!page||attached.contains(page))continue;
            auto* box=qobject_cast<QBoxLayout*>(target);auto* grid=qobject_cast<QGridLayout*>(target);auto* form=qobject_cast<QFormLayout*>(target);
            auto* panel=new Panel(page,dialog);
            if(box){int index=box->count();for(int i=0;i<box->count();++i)if(box->itemAt(i)->spacerItem()){index=i;break;}box->insertWidget(index,panel);}
            else if(grid){grid->addWidget(panel,grid->rowCount(),0,1,std::max(1,grid->columnCount()));}
            else if(form){form->addRow(panel);}
            attached.insert(page);panels.push_back(panel);connect(panel,&QObject::destroyed,this,[this,page]{attached.remove(page);});
        }
    }
};
Controller* controller=nullptr;
}
extern "C" __declspec(dllexport) int NingqNoiseUiStart(const NingqNoiseUiCallbacks* api){
    try{
        if(!api||api->size!=sizeof(*api)||!api->read||!api->write||!api->status||!qApp||std::strcmp(qVersion(),"5.15.2")!=0)return 0;
        if(QThread::currentThread()!=qApp->thread())return 0;
        if(controller)return 1;callbacks=*api;controller=new Controller;return 1;
    }catch(...){return 0;}
}
extern "C" __declspec(dllexport) void NingqNoiseUiStop(){
    if(!controller&&!tutorial)return;
    auto cleanup=[]{delete tutorial;tutorial=nullptr;delete controller;controller=nullptr;};
    if(QThread::currentThread()==qApp->thread())cleanup();
    else QMetaObject::invokeMethod(qApp,cleanup,Qt::BlockingQueuedConnection);
}
extern "C" __declspec(dllexport) int NingqTutorialStart(const wchar_t* profileDirectory,const wchar_t* imagePath,void(*openAudio)()){
    try{
        (void)imagePath; // Reserved ABI parameter; tutorial is text-only.
        if(!controller||!qApp||QThread::currentThread()!=qApp->thread()||!profileDirectory||!*profileDirectory)return 0;
        if(tutorial)return 1;
        tutorial=new TutorialController(QString::fromWCharArray(profileDirectory),openAudio);return 1;
    }catch(...){return 0;}
}
extern "C" __declspec(dllexport) void NingqTutorialShow(){
    if(!tutorial)return;
    if(QThread::currentThread()==qApp->thread())tutorial->showGuide();
    else QMetaObject::invokeMethod(tutorial,[]{if(tutorial)tutorial->showGuide();},Qt::QueuedConnection);
}
