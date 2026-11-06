// SPDX-License-Identifier: MIT
// Controlled GPU-completion tickets exercise the production presenter and real keyed mutexes.
#define KOE_DIRECT_FLOW
#include "../stock/FrameGeneration/NativeFrameGeneration.cpp"
#include <iostream>
#include <limits>

static unsigned checks,busyCalls;
static HWND fixtureForeground{};
static HWND WINAPI ForegroundWindow(){return fixtureForeground;}
static void Require(bool value,const char *message)
{++checks;if(!value)throw std::runtime_error(message);}
static HRESULT STDMETHODCALLTYPE BusyPresent(IDXGISwapChain*,UINT interval,UINT flags)
{
    Require(interval==0,"Generated presentation requested a blocking vsync interval");
    Require((flags&DXGI_PRESENT_DO_NOT_WAIT)!=0,"Generated presentation omitted DO_NOT_WAIT");
    busyCalls++;return DXGI_ERROR_WAS_STILL_DRAWING;
}
static std::shared_ptr<Interpolator::Ticket> Ticket(Presenter &presenter,bool pending=false)
{
    auto ticket=std::make_shared<Interpolator::Ticket>();ticket->owner=presenter.fruc.get();
    ticket->fenceValue=pending?std::numeric_limits<std::uint64_t>::max():0;
    ticket->gpuConditional=true;ticket->eligible=true;ticket->rejection=Interpolator::Accepted;
    ticket->counts[3]=presenter.source->desc.Width*presenter.source->desc.Height;
    ticket->counts[8]=1;ticket->counts[9]=2;ticket->counts[10]=3;ticket->counts[11]=4;
    return ticket;
}
static Presenter::GpuFrame &Queue(Presenter &presenter,bool pending=false)
{
    auto &frame=presenter.frames[0];frame=Presenter::GpuFrame{};
    double now=Seconds();frame.timestamp=now-.001;frame.beforeTimestamp=now-.08;
    frame.deadline=now+.5;frame.serial=1;frame.ticket=Ticket(presenter,pending);frame.phaseCount=1;
    frame.phases.resize(1);frame.phases[0].ticket=Ticket(presenter,pending);
    frame.phases[0].sampleTime=FrameTimeline::SampleTime(frame.beforeTimestamp,frame.timestamp,1,1);
    presenter.readyFrames.push_back(0);return frame;
}
static void ReadyInput(Shared &source,unsigned index,std::uint64_t serial,double timestamp)
{
    auto &slot=source.slots[index];
    Require(slot.mutex->AcquireSync(0,0)==S_OK,"Fixture world ownership was unavailable");
    Require(slot.finalMutex->AcquireSync(0,0)==S_OK,"Fixture UI ownership was unavailable");
    Require(slot.silhouetteMutex->AcquireSync(0,0)==S_OK,"Fixture silhouette ownership was unavailable");
    Check(slot.silhouetteMutex->ReleaseSync(1),"fixture silhouette ready");
    Check(slot.finalMutex->ReleaseSync(1),"fixture UI ready");
    Check(slot.mutex->ReleaseSync(1),"fixture world ready");
    slot.status=2;slot.serial=serial;slot.timestamp=timestamp;
}
int main()
{
    HWND previous=GetForegroundWindow(),sourceWindow{};
    try
    {
        SetEnvironmentVariableW(L"KOE_INDEPENDENT_LAYERS",L"0");
        SetEnvironmentVariableW(L"KOE_INDEPENDENT_SNAPSHOT",nullptr);
        WNDCLASSW type{};type.lpfnWndProc=DefWindowProcW;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"KoeQueueFixture";
        RegisterClassW(&type);sourceWindow=CreateWindowW(type.lpszClassName,L"KOE queue unit test",WS_POPUP,0,0,128,96,nullptr,nullptr,type.hInstance,nullptr);
        Require(sourceWindow!=nullptr,"Queue fixture could not create its own window");
        fixtureForeground=sourceWindow;
        DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=128;desc.BufferDesc.Height=96;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.OutputWindow=sourceWindow;desc.Windowed=TRUE;
        ComPtr<ID3D11Device> gpu;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> swap;
        Check(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,&swap,&gpu,nullptr,&context),"fixture device");
        ComPtr<ID3D11Texture2D> image;Check(swap->GetBuffer(0,IID_PPV_ARGS(&image)),"fixture source image");
        auto source=CreateShared(swap.Get(),image.Get(),sourceWindow);enabled=true;allowed=true;
        ComPtr<IDXGIDevice1> sourceLatency;Check(gpu.As(&sourceLatency),"fixture source latency interface");
        UINT originalLatency{};Check(sourceLatency->GetMaximumFrameLatency(&originalLatency),"fixture original latency");
        {
            Presenter presenter(source);presenter.foregroundWindow=&ForegroundWindow;source->lastCaptured=Seconds();
            UINT unchangedLatency{};Check(sourceLatency->GetMaximumFrameLatency(&unchangedLatency),"fixture preserved latency");
            Require(unchangedLatency==originalLatency,"The generator changed the game's presentation queue limit");
            auto &blocked=Queue(presenter,true);blocked.deadline=Seconds()-.1;
            presenter.Show();presenter.displaying=true;
            Require(IsWindowVisible(presenter.window)!=FALSE,"Fixture overlay did not become visible");
            presenter.ResolveFrames();
            Require(!IsWindowVisible(presenter.window) && !presenter.displaying,"A late GPU frame held the visible overlay");
            Require(blocked.ticket && presenter.readyFrames.size()==1,"An unfinished GPU buffer was released for reuse");
            blocked.ticket->fenceValue=0;blocked.phases[0].ticket->fenceValue=0;
            presenter.ResolveFrames();
            Require(presenter.readyFrames.empty() && !blocked.ticket,"An expired completed frame was not retired");

            auto &old=Queue(presenter);source->lastCaptured=old.timestamp;source->latestWorldSerial=2;
            auto realBefore=realPresents.load();presenter.ResolveFrames();
            Require(!presenter.displaying && realPresents.load()==realBefore,"A stale real frame anchored over a newer source");
            Require(presenter.readyFrames.empty(),"An unanchorable completed frame stayed queued");

            auto &duplicate=Queue(presenter);source->lastCaptured=Seconds();presenter.displaying=true;
            presenter.timeline.Shown(duplicate.beforeTimestamp);presenter.lastHashKnown=true;
            presenter.lastPresentedHash=(std::uint64_t(1)<<32)|2;
            presenter.presentationClock.Presented(Seconds(),1);
            auto generatedBefore=generatedPresents.load();presenter.ResolveFrames();
            Require(duplicate.nextPhase==1 && generatedPresents.load()==generatedBefore,"A duplicate image entered presentation");
            Require(duplicate.ticket!=nullptr,"Pacing destroyed a real frame before it could be considered");
            duplicate.deadline=Seconds()-.1;presenter.ResolveFrames();

            auto &reordered=Queue(presenter);source->lastCaptured=Seconds();presenter.displaying=true;
            presenter.timeline.Shown(reordered.timestamp+.0001);presenter.lastHashKnown=false;
            presenter.ResolveFrames();
            Require(generatedPresents.load()==generatedBefore && presenter.readyFrames.empty(),"A completed older pose rolled the timeline backward");

            presenter.frames[0].ticket=Ticket(presenter,true);presenter.frames[1].ticket=Ticket(presenter,true);
            Require(!presenter.ProcessQueued(0,Seconds(),.1),"A full queue overwrote an in-flight GPU frame");
            presenter.frames[0].ticket.reset();presenter.frames[1].ticket.reset();

            ReadyInput(*source,0,3,Seconds());
            Require(source->slots[0].finalMutex->AcquireSync(1,0)==S_OK,"Fixture could not hold the second mutex");
            Require(!presenter.ProcessQueued(0,source->slots[0].timestamp,.1),"A busy input mutex was treated as acquired");
            Require(source->slots[0].mutex->AcquireSync(1,0)==S_OK,"Partial acquisition did not restore the first mutex key");
            Check(source->slots[0].mutex->ReleaseSync(1),"fixture world retry");
            Check(source->slots[0].finalMutex->ReleaseSync(1),"fixture UI retry");
            Require(presenter.ProcessQueued(0,Seconds()-1,.1),"Expired input was retried instead of discarded");
            Require(source->slots[0].status==0 && presenter.readyFrames.empty(),"Expired input performed GPU generation");

            presenter.lastInputSerial=10;presenter.lastTimestamp=Seconds()-.05;
            ReadyInput(*source,0,9,Seconds());
            Require(presenter.ProcessQueued(0,source->slots[0].timestamp,.1),"Old serial was retried forever");
            Require(source->slots[0].status==0 && presenter.lastInputSerial==10 && presenter.readyFrames.empty(),"Out-of-order input changed generation history");

            auto initialized=MH_Initialize();Require(initialized==MH_OK || initialized==MH_ERROR_ALREADY_INITIALIZED,"Present fixture hook initialization failed");
            auto table=*reinterpret_cast<void***>(presenter.swap.Get());void *original{};
            Require(MH_CreateHook(table[8],reinterpret_cast<void*>(&BusyPresent),&original)==MH_OK,"Busy Present hook creation failed");
            Require(MH_EnableHook(table[8])==MH_OK,"Busy Present hook activation failed");
            source->lastCaptured=Seconds();presenter.Show();
            double displayDeadline=Seconds()+2;
            presenter.PreparePresentation(presenter.finalImage.Get(),true,true,displayDeadline);
            bool submitted=false;
            Require(presenter.copyFenceValue!=0 && busyCalls==0,"Preparing a GPU copy submitted an image to Present");
            auto copyValue=presenter.copyFenceValue;
            presenter.copyFenceValue=std::numeric_limits<std::uint64_t>::max()-1;
            Require(!presenter.Present(presenter.finalImage.Get(),true,true,displayDeadline)
                && presenter.pendingPresentation && busyCalls==0,"An unfinished copy entered Present or waited synchronously");
            ReadyInput(*source,0,11,Seconds());
            Require(!presenter.ProcessQueued(0,source->slots[0].timestamp,.1),"New generation was queued ahead of a pending presentation copy");
            Require(source->slots[0].mutex->AcquireSync(1,0)==S_OK,"Presentation backpressure consumed the next source image");
            Check(source->slots[0].mutex->ReleaseSync(1),"fixture pending source retry");
            presenter.copyFenceValue=copyValue;
            do
            {
                source->lastCaptured=Seconds();
                submitted=presenter.Present(presenter.finalImage.Get(),true,true,displayDeadline);
                if(!presenter.pendingPresentation)break;
                SwitchToThread();
            }while(Seconds()<displayDeadline);
            Require(!presenter.Present(presenter.finalImage.Get(),true,true,Seconds()-.01) && busyCalls==1,
                "An already expired image reached DXGI Present");
            MH_DisableHook(table[8]);MH_RemoveHook(table[8]);
            Require(!submitted && busyCalls==1,"A busy presentation blocked, threw, or counted an undisplayed image");
            presenter.Hide();
        }
        DestroyWindow(sourceWindow);if(previous)SetForegroundWindow(previous);
        std::cout<<"Frame queue: "<<checks<<" production presenter checks passed.\n";return 0;
    }
    catch(const std::exception &error)
    {
        if(sourceWindow)DestroyWindow(sourceWindow);if(previous)SetForegroundWindow(previous);
        std::cerr<<"Frame queue: "<<error.what()<<"\n";return 1;
    }
}
