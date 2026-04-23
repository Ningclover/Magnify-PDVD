#include "GuiController.h"
#include "MainWindow.h"
#include "ViewWindow.h"
#include "ControlWindow.h"
#include "Data.h"
#include "Waveforms.h"
#include "RawWaveforms.h"
#include "BadChannels.h"

#include "TApplication.h"
#include "TSystem.h"
#include "TExec.h"
#include "TROOT.h"
#include "TMath.h"
#include "TGFileDialog.h"

#include "TGMenu.h"
#include "TGNumberEntry.h"
#include "TCanvas.h"
#include "TH2F.h"
#include "TH1F.h"
#include "TH2I.h"
#include "TH1I.h"
#include "TBox.h"
#include "TLine.h"
#include "TColor.h"
#include "TStyle.h"
#include "TLegend.h"
#include "TGButton.h"
#include "TGLabel.h"
#include "TRootEmbeddedCanvas.h"
#include "TGClient.h"

#include <iostream>
#include <vector>
#include <algorithm>
#include <string>
using namespace std;


GuiController::GuiController(const TGWindow *p, int w, int h, const char* fn, double threshold, const char* frame, int rebin)
{
    mw = new MainWindow(p, w, h);
    vw = mw->fViewWindow;
    cw = mw->fControlWindow;

    // data = new Data("../data/2D_display_3455_0_0.root");
    TString filename;
    if (!fn) {
        filename = OpenDialog();
    }
    else {
        filename = fn;
    }
    data = new Data(filename.Data(), threshold, frame, rebin);
    captureMode = CAPTURE_NONE;
    regionWindow = nullptr;
    regionCanvas = nullptr;
    for (int p = 0; p < 3; ++p) {
        regChStart[p] = regChEnd[p] = nullptr;
        regTLowS[p]  = regTHighS[p]  = nullptr;
        regTLowE[p]  = regTHighE[p]  = nullptr;
        for (int e = 0; e < 4; ++e) regionBoundary[p][e] = nullptr;
    }
    mw->SetWindowName(TString::Format("Magnify: run %i, sub-run %i, event %i, anode %i",
        data->runNo, data->subRunNo, data->eventNo, data->anodeNo));

    for (int i=0; i<6; i++) {
        vw->can->cd(i+1);
        data->wfs.at(i)->Draw2D();
    }
    for (int i=0; i<3; i++) {
        vw->can->cd(i+7);
        int chanNo = data->wfs.at(i)->firstChannel;
        std::string comment = data->channel_status[chanNo];
        data->wfs.at(i)->Draw1D(chanNo, "", comment.c_str());
        TH1F *h = data->wfs.at(i+3)->Draw1D(chanNo, "same"); // draw calib
        h->SetLineColor(kRed);
        hCurrent[i] = h;
    }

    InitConnections();
}

GuiController::~GuiController()
{
    // gApplication->Terminate(0);
}

void GuiController::InitConnections()
{
    mw->fMenuFile->Connect("Activated(int)", "GuiController", this, "HandleMenu(int)");

    for (int i=0; i<3; i++) {
        cw->threshEntry[i]->SetNumber(data->wfs.at(i)->threshold);
    }
    cw->threshEntry[0]->Connect("ValueSet(Long_t)", "GuiController", this, "ThresholdUChanged()");
    cw->threshEntry[1]->Connect("ValueSet(Long_t)", "GuiController", this, "ThresholdVChanged()");
    cw->threshEntry[2]->Connect("ValueSet(Long_t)", "GuiController", this, "ThresholdWChanged()");
    cw->setThreshButton->Connect("Clicked()", "GuiController", this, "SetChannelThreshold()");
    cw->threshScaleEntry->Connect("ValueSet(Long_t)", "GuiController", this, "SetChannelThreshold()");

    cw->zAxisRangeEntry[0]->SetNumber(data->wfs.at(0)->zmin);
    cw->zAxisRangeEntry[1]->SetNumber(data->wfs.at(0)->zmax);
    cw->zAxisRangeEntry[0]->Connect("ValueSet(Long_t)", "GuiController", this, "ZRangeChanged()");
    cw->zAxisRangeEntry[1]->Connect("ValueSet(Long_t)", "GuiController", this, "ZRangeChanged()");

    cw->timeRangeEntry[0]->SetNumber(0);
    cw->timeRangeEntry[1]->SetNumber(data->wfs.at(0)->nTDCs);

    cw->channelEntry->Connect("ValueSet(Long_t)", "GuiController", this, "ChannelChanged()");
    cw->timeEntry->Connect("ValueSet(Long_t)", "GuiController", this, "TimeChanged()");
    cw->badChanelButton->Connect("Clicked()", "GuiController", this, "UpdateShowBadChannel()");
    cw->badChanelButton->SetToolTipText(TString::Format("U: %lu, V: %lu, Y: %lu",
        data->wfs.at(0)->lines.size(),
        data->wfs.at(1)->lines.size(),
        data->wfs.at(2)->lines.size()
    ));
    cw->rawWfButton->Connect("Clicked()", "GuiController", this, "UpdateShowRaw()");
    cw->unZoomButton->Connect("Clicked()", "GuiController", this, "UnZoom()");

    cw->regionSumBtn->Connect("Clicked()", "GuiController", this, "ShowRegionWindow()");

    // stupid way to connect signal and slots
    vw->can->GetPad(1)->Connect("RangeChanged()", "GuiController", this, "SyncTimeAxis0()");
    vw->can->GetPad(2)->Connect("RangeChanged()", "GuiController", this, "SyncTimeAxis1()");
    vw->can->GetPad(3)->Connect("RangeChanged()", "GuiController", this, "SyncTimeAxis2()");
    vw->can->GetPad(4)->Connect("RangeChanged()", "GuiController", this, "SyncTimeAxis3()");
    vw->can->GetPad(5)->Connect("RangeChanged()", "GuiController", this, "SyncTimeAxis4()");
    vw->can->GetPad(6)->Connect("RangeChanged()", "GuiController", this, "SyncTimeAxis5()");
    // vw->can->GetPad(7)->Connect("RangeChanged()", "GuiController", this, "WfRangeChanged0()");
    // vw->can->GetPad(8)->Connect("RangeChanged()", "GuiController", this, "WfRangeChanged1()");
    // vw->can->GetPad(9)->Connect("RangeChanged()", "GuiController", this, "WfRangeChanged2()");


    vw->can->Connect(
        "ProcessedEvent(Int_t,Int_t,Int_t,TObject*)",
        "GuiController",
        this,
        "ProcessCanvasEvent(Int_t,Int_t,Int_t,TObject*)"
    );
}

void GuiController::UpdateShowBadChannel()
{
    if (cw->badChanelButton->IsDown()) {
        for (int ind=0; ind<6; ind++) {
            vw->can->cd(ind+1);
            data->wfs.at(ind)->DrawLines();
            vw->can->GetPad(ind+1)->Modified();
            vw->can->GetPad(ind+1)->Update();
        }
    }
    else {
        for (int ind=0; ind<6; ind++) {
            vw->can->cd(ind+1);
            data->wfs.at(ind)->HideLines();
            vw->can->GetPad(ind+1)->Modified();
            vw->can->GetPad(ind+1)->Update();
        }
    }

}

void GuiController::ThresholdChanged(int i)
{
    // newThresh is an ADC-unit cutoff (widget is seeded from denoised display units
    // where fScale=1, so widget value == ADC value).  Apply it as an ADC cutoff to
    // all waveforms for this plane so both denoised and decon cut at the same ADC
    // level: pass newThresh * fScale so that |hOrig*fScale| > newThresh*fScale
    // reduces to |hOrig_ADC| > newThresh.
    // Widget value is the denoised ADC threshold.  Decon is kept at a fixed ratio
    // (decon_scaling / denoised_scaling) higher in ADC space, matching the startup
    // per-channel relationship.
    int newThresh = cw->threshEntry[i]->GetNumber();
    double scalingRatio = data->decon_scaling / data->denoised_scaling;
    for (int ind=i; ind<6; ind+=3) {
        vw->can->cd(ind+1);
        double fS = data->wfs.at(ind)->fScale;
        double adcCutoff = (ind < 3) ? newThresh : newThresh * scalingRatio;
        double displayThresh = adcCutoff * fS;
        data->wfs.at(ind)->SetThreshold(displayThresh);
        data->wfs.at(ind)->Draw2D();
        vw->can->GetPad(ind+1)->Modified();
        vw->can->GetPad(ind+1)->Update();
    }
}

void GuiController::SetChannelThreshold()
{
    // Reset all 6 waveforms (denoised + decon for all 3 planes) to per-channel
    // Wiener thresholds.  Denoised always uses scaling=0.5; decon uses the
    // user-entered multiplier from threshScaleEntry.
    TH1I *ht = 0;
    double decon_scale = cw->threshScaleEntry->GetNumber();
    for (int ind=0; ind<6; ind++) {
        vw->can->cd(ind+1);
        ht = data->thresh_histos.at(ind % 3);
        double scaling = (ind < 3) ? data->denoised_scaling : decon_scale;
        data->wfs.at(ind)->SetThreshold(ht, scaling);
        // update the widget to reflect the new denoised threshold
        if (ind < 3) cw->threshEntry[ind]->SetNumber(data->wfs.at(ind)->threshold);
        data->wfs.at(ind)->Draw2D();
        vw->can->GetPad(ind+1)->Modified();
        vw->can->GetPad(ind+1)->Update();
    }
}

void GuiController::UnZoom()
{
    cw->timeRangeEntry[0]->SetNumber(0);
    cw->timeRangeEntry[1]->SetNumber(data->wfs.at(0)->nTDCs);
    cw->adcRangeEntry[0]->SetNumber(0);
    cw->adcRangeEntry[1]->SetNumber(0);

    for (int ind=0; ind<6; ind++) {
        data->wfs.at(ind)->hDummy->GetXaxis()->UnZoom();
        data->wfs.at(ind)->hDummy->GetYaxis()->UnZoom();
        vw->can->GetPad(ind+1)->Modified();
        vw->can->GetPad(ind+1)->Update();
    }
}

void GuiController::ZRangeChanged()
{
    int min = cw->zAxisRangeEntry[0]->GetNumber();
    int max = cw->zAxisRangeEntry[1]->GetNumber();
    for (int ind=0; ind<6; ind++) {
        vw->can->cd(ind+1);
        data->wfs.at(ind)->SetZRange(min, max);
        data->wfs.at(ind)->Draw2D();
        vw->can->GetPad(ind+1)->Modified();
        vw->can->GetPad(ind+1)->Update();
    }

}

void GuiController::SyncTimeAxis(int i)
{
    double min = data->wfs.at(i)->hDummy->GetYaxis()->GetFirst();
    double max = data->wfs.at(i)->hDummy->GetYaxis()->GetLast();
    // double min = vw->can->GetPad(i+1)->GetUymin();
    // double max = vw->can->GetPad(i+1)->GetUymax();

    for (int ind=0; ind<6; ind++) {
        if (i==ind) continue;
        data->wfs.at(ind)->hDummy->GetYaxis()->SetRange(min, max);
        vw->can->GetPad(ind+1)->Modified();
        vw->can->GetPad(ind+1)->Update();
    }

    // cout << "range changed: " << min << ", " << max << endl;
}

void GuiController::WfRangeChanged(int i)
{
    // can't figureout how to get the axis range in user coordinate ...
    // ( not pad->GetUxmin() etc, nor axis->GetFirst() etc. )
}

void GuiController::UpdateShowRaw()
{
    int channel = cw->channelEntry->GetNumber();
    cout << "channel: " << channel << endl;
    // int wfsNo = 0;
    // if (channel>=data->wfs.at(1)->firstChannel && channel<data->wfs.at(2)->firstChannel) wfsNo = 1;
    // else if (channel>=data->wfs.at(2)->firstChannel) wfsNo = 2;

    int wfsNo = data->GetPlaneNo(channel);
    cout << "plane: " << data->GetPlaneNo(channel) << endl;

    int padNo = wfsNo+7;
    vw->can->cd(padNo);

    TH1I *hh = data->raw_wfs.at(wfsNo)->Draw1D(channel, "same");
    if (cw->rawWfButton->IsDown()) {
        hh->SetLineColor(kBlue);
        // hMain->SetTitle( TString::Format("%s, %s", hMain->GetTitle(), hh->GetTitle()) );
    }
    else {
        gPad->GetListOfPrimitives()->Remove(hh);
        // hMain->SetTitle( TString::Format("%s, %s", hMain->GetTitle(), hh->GetTitle()) );
    }

    vw->can->GetPad(padNo)->Modified();
    vw->can->GetPad(padNo)->Update();
}

void GuiController::ChannelChanged()
{
    if (cw->timeModeButton->IsDown()) {
        return; // skip if time mode is selected
    }
    if (cw->badOnlyButton->IsDown()){
        int curr = cw->channelEntry->GetNumber();
        int wfsNum = data->GetPlaneNo(curr);
        BadChannels* bad_channels = data->wfs.at(wfsNum)->bad_channels;
        vector<int> bad_id = bad_channels->bad_id;
        int next = 0;
        auto it = std::find(bad_id.begin(), bad_id.end(), curr);
        if (it!=bad_id.end()){
            next = *it;
        }
        else{
            it = std::upper_bound(bad_id.begin(), bad_id.end(), curr); // find first element greater
            if(it!=bad_id.end()){
                next = *it;
            }            
        }
        cw->channelEntry->SetNumber(next);
    }

    int channel = cw->channelEntry->GetNumber();
    cout << "channel: " << channel << endl;
    // int wfsNo = 0;
    // if (channel>=data->wfs.at(1)->firstChannel && channel<data->wfs.at(2)->firstChannel) wfsNo = 1;
    // else if (channel>=data->wfs.at(2)->firstChannel) wfsNo = 2;
    int wfsNo = data->GetPlaneNo(channel);
    cout << "plane: " << data->GetPlaneNo(channel) << endl;

    int padNo = wfsNo+7;
    vw->can->cd(padNo);

    std::string comment = data->channel_status[channel];
    TH1F *hwf = data->wfs.at(wfsNo)->Draw1D(channel, "", comment.c_str());
    hCurrent[wfsNo] = hwf;
    hwf->SetLineColor(kBlack);


    TString name = TString::Format("hWire_%s_2d_dummy", data->wfs.at(wfsNo)->fName.Data());
    TH2F *hMain = (TH2F*)gDirectory->FindObject(name);
    if (!hMain) {
        cout << "Error: cannot find " << name << endl;
        return;
    }

    hMain->GetXaxis()->SetRangeUser(cw->timeRangeEntry[0]->GetNumber(), cw->timeRangeEntry[1]->GetNumber());
    if (binary_search(data->bad_channels->bad_id.begin(), data->bad_channels->bad_id.end(), channel)) {
        hMain->SetTitle( TString::Format("%s (bad channel)", hMain->GetTitle()) );
    }

    TH1F *h = data->wfs.at(wfsNo+3)->Draw1D(channel, "same" ); // draw decon (red)
    h->SetLineColor(kRed);

    TH1I *ht = data->thresh_histos.at(wfsNo);
    int thresh = ht->GetBinContent(ht->GetXaxis()->FindBin(channel));
    cout << "thresh: " << thresh << endl;
    TLine *l = new TLine(0, thresh/500., data->wfs.at(wfsNo)->nTDCs, thresh/500.);
    l->SetLineColor(kMagenta);
    l->SetLineWidth(2);
    l->Draw();

    TH1I *hh = nullptr;
    if (cw->rawWfButton->IsDown()) {
        hh = data->raw_wfs.at(wfsNo)->Draw1D(channel, "same");
        hh->SetLineColor(kBlue);
        hMain->SetTitle( TString::Format("%s, %s", hMain->GetTitle(), hh->GetTitle()) );
    }

    // Smart Y-axis range: derive from actual signal rather than the denoised
    // waveform which may be empty. Manual ADC range entries override auto-range.
    {
        int adc_min = cw->adcRangeEntry[0]->GetNumber();
        int adc_max = cw->adcRangeEntry[1]->GetNumber();
        if (adc_max > adc_min) {
            hMain->GetYaxis()->SetRangeUser(adc_min, adc_max);
        } else {
            // Start from decon waveform (primary signal source)
            double ylo = h->GetMinimum(), yhi = h->GetMaximum();
            // Expand to include denoised if it has real content
            if (hwf->GetMaximum() > hwf->GetMinimum()) {
                ylo = std::min(ylo, hwf->GetMinimum());
                yhi = std::max(yhi, hwf->GetMaximum());
            }
            // Expand to include raw if it is visible
            if (hh && hh->GetMaximum() > hh->GetMinimum()) {
                ylo = std::min(ylo, (double)hh->GetMinimum());
                yhi = std::max(yhi, (double)hh->GetMaximum());
            }
            if (yhi > ylo) {
                double pad = 0.1 * (yhi - ylo);
                hMain->GetYaxis()->SetRangeUser(ylo - pad, yhi + pad);
            } else {
                // Flat / empty channel — show a small symmetric range
                hMain->GetYaxis()->SetRangeUser(-1.0, 1.0);
            }
        }
    }

    // mask the bad channel region
    if (cw->badChanelButton->IsDown()){
        BadChannels* bad_channels = data->wfs.at(wfsNo)->bad_channels;
        vector<int> bad_id = bad_channels->bad_id;
        int idx=0;
        for(auto& ch: bad_id){
            if(ch==channel){
                vector<int> bad_start = bad_channels->bad_start;
                vector<int> bad_end = bad_channels->bad_end;
                TLine* lh = new TLine(bad_start.at(idx), hwf->GetMinimum(), bad_start.at(idx), hwf->GetMaximum());
                TLine* rh = new TLine(bad_end.at(idx), hwf->GetMinimum(), bad_end.at(idx), hwf->GetMaximum());
                lh->SetLineColor(2); lh->SetLineStyle(2); lh->SetLineWidth(2);
                rh->SetLineColor(2); rh->SetLineStyle(2); rh->SetLineWidth(2);
                lh->Draw("same");
                rh->Draw("same");
                cout << "find a bad channel :" << channel << " start: " << bad_start.at(idx) << " end: " << bad_end.at(idx) << endl;

                TBox* breg = new TBox(bad_start.at(idx), hwf->GetMinimum(), bad_end.at(idx), hwf->GetMaximum());
                breg->SetFillStyle(3335);
                breg->SetFillColor(kRed);
                // gStyle->SetHatchesSpacing(5);
                breg->Draw("same");
            }
            idx ++;
        }
    }

    vw->can->GetPad(padNo)->SetGridx();
    vw->can->GetPad(padNo)->SetGridy();
    vw->can->GetPad(padNo)->Modified();
    vw->can->GetPad(padNo)->Update();
    // if (cw->badOnlyButton->IsDown()){ // evil mode & print figures
    //     std:string pwd(gSystem->WorkingDirectory());
    //     pwd += "/../data/Channel" + std::to_string(channel) + ".png";
    //     vw->can->GetPad(padNo)->Print(pwd.c_str());
    //     // std::cerr << "[wgu] print a channel" << channel << " path: " << pwd << endl;
    // }
}

void GuiController::TimeChanged()
{
    if (cw->timeModeButton->IsDown()) {

        int tickNo = cw->timeEntry->GetNumber();
        TH1F *hTick  = 0;
        for (int k=3; k<=5; k++) { // only draw decon signal
            int padNo = k+4;
            vw->can->cd(padNo);
            hTick = data->wfs.at(k)->Draw1DTick(tickNo); // draw time
            hTick->SetLineColor(kRed);

            TString name = TString::Format("hth_%i", k-3);
            TH1I *hth = (TH1I*)gDirectory->FindObject(name);
            if (hth) delete hth;

            hth = (TH1I*)data->thresh_histos.at(k-3)->Clone(name.Data());
            hth->Scale(data->wfs.at(k)->fScale);
            hth->Draw("same");
            hth->SetLineColor(kBlack);

            int channel_min = cw->timeRangeEntry[0]->GetNumber();
            int channel_max = cw->timeRangeEntry[1]->GetNumber();

            if (channel_min>0) {
                hTick->GetXaxis()->SetRangeUser(channel_min, channel_max);
            }

            int adc_min = cw->adcRangeEntry[0]->GetNumber();
            int adc_max = cw->adcRangeEntry[1]->GetNumber();
            if (adc_max > adc_min) {
                hTick->GetYaxis()->SetRangeUser(adc_min, adc_max);
            }

            vw->can->GetPad(padNo)->SetGridx();
            vw->can->GetPad(padNo)->SetGridy();
            vw->can->GetPad(padNo)->Modified();
            vw->can->GetPad(padNo)->Update();
        }
    }
}

void GuiController::ProcessCanvasEvent(Int_t ev, Int_t x, Int_t y, TObject *selected)
{
    if (ev == 11) { // clicked
        if (!(selected->IsA() == TH2F::Class()
            || selected->IsA() == TBox::Class()
            || selected->IsA() == TLine::Class()
        )) return;
        TVirtualPad* pad = vw->can->GetClickSelectedPad();
        int padNo = pad->GetNumber();
        double xx = pad->AbsPixeltoX(x);
        double yy = pad->AbsPixeltoY(y);
        cout << "pad " << padNo << ": (" << xx << ", " << yy << ")" << endl;

        // Region-sum capture mode: fill start/end fields on click in decon pads (4-6)
        if (captureMode != CAPTURE_NONE && padNo >= 4 && padNo <= 6) {
            if (!regionWindow) { captureMode = CAPTURE_NONE; return; }
            int plane = padNo - 4;
            int chanNo = TMath::Nint(xx);
            int tickNo = TMath::Nint(yy);
            if (captureMode == CAPTURE_START) {
                regChStart[plane]->SetNumber(chanNo);
                regTLowS[plane]->SetNumber(tickNo);
                regTHighS[plane]->SetNumber(tickNo);
                cout << "Set Start: plane " << plane << "  ch=" << chanNo << "  tick=" << tickNo << endl;
            } else {
                regChEnd[plane]->SetNumber(chanNo);
                regTLowE[plane]->SetNumber(tickNo);
                regTHighE[plane]->SetNumber(tickNo);
                cout << "Set End: plane " << plane << "  ch=" << chanNo << "  tick=" << tickNo << endl;
            }
            return;  // keep mode active; user clicks Set Start/End again to exit
        }

        int drawPad = (padNo-1) % 3 + 7;
        vw->can->cd(drawPad);
        if (padNo<=6) {
            int wfNo = padNo - 1;
            wfNo = wfNo < 3 ? wfNo : wfNo-3;  // draw raw first
            int chanNo = TMath::Nint(xx); // round
            int tickNo = TMath::Nint(yy); // round
            // data->wfs.at(wfNo)->Draw1D(chanNo);
            // TH1F *h = data->wfs.at(wfNo+3)->Draw1D(chanNo, "same"); // draw calib
            // h->SetLineColor(kRed);
            // TH1I *hh = data->raw_wfs.at(wfNo)->Draw1D(chanNo, "same"); // draw calib
            // hh->SetLineColor(kBlue);
            cw->channelEntry->SetNumber(chanNo);
            cw->timeEntry->SetNumber(tickNo);

            ChannelChanged();
            TimeChanged();

            // cw->timeRangeEntry[0]->SetNumber(0);
            // cw->timeRangeEntry[1]->SetNumber(data->wfs.at(0)->nTDCs);
        }
        // vw->can->GetPad(drawPad)->Modified();
        // vw->can->GetPad(drawPad)->Update();
    }

}

void GuiController::ShowRegionWindow()
{
    if (!regionWindow) {
        regionWindow = new TGMainFrame(gClient->GetRoot(), 700, 520);
        regionWindow->SetWindowName("Region Sum");
        regionWindow->DontCallClose();
        regionWindow->Connect("CloseWindow()", "GuiController", this, "HideRegionWindow()");

        // ---- Control group ----
        TGGroupFrame* ctrl = new TGGroupFrame(regionWindow, "Region Selection", kVerticalFrame);
        regionWindow->AddFrame(ctrl, new TGLayoutHints(kLHintsTop | kLHintsExpandX, 5, 5, 5, 2));

        // Header row
        {
            TGHorizontalFrame* hdr = new TGHorizontalFrame(ctrl);
            ctrl->AddFrame(hdr, new TGLayoutHints(kLHintsTop | kLHintsLeft, 2, 2, 2, 0));
            hdr->AddFrame(new TGLabel(hdr, "      "),  new TGLayoutHints(kLHintsLeft, 2, 2, 1, 1));
            hdr->AddFrame(new TGLabel(hdr, " ch start"), new TGLayoutHints(kLHintsLeft, 18, 2, 1, 1));
            hdr->AddFrame(new TGLabel(hdr, "  t start low  -  high"), new TGLayoutHints(kLHintsLeft, 2, 2, 1, 1));
            hdr->AddFrame(new TGLabel(hdr, "  ch end"), new TGLayoutHints(kLHintsLeft, 18, 2, 1, 1));
            hdr->AddFrame(new TGLabel(hdr, "  t end low  -  high"), new TGLayoutHints(kLHintsLeft, 2, 2, 1, 1));
        }

        static const char* planeName[3] = {"U:", "V:", "W:"};
        for (int p = 0; p < 3; ++p) {
            TGHorizontalFrame* row = new TGHorizontalFrame(ctrl);
            ctrl->AddFrame(row, new TGLayoutHints(kLHintsTop | kLHintsLeft, 2, 2, 3, 3));

            row->AddFrame(new TGLabel(row, planeName[p]),
                new TGLayoutHints(kLHintsLeft | kLHintsCenterY, 5, 5, 2, 2));

            // ch_start
            regChStart[p] = new TGNumberEntry(row, 0, 6, -1, TGNumberFormat::kNESInteger,
                TGNumberFormat::kNEANonNegative, TGNumberFormat::kNELLimitMinMax, 0, 15359);
            row->AddFrame(regChStart[p], new TGLayoutHints(kLHintsLeft, 5, 2, 1, 1));

            // t_low_s - t_high_s
            regTLowS[p] = new TGNumberEntry(row, 0, 6, -1, TGNumberFormat::kNESInteger,
                TGNumberFormat::kNEANonNegative, TGNumberFormat::kNELLimitMinMax, 0, 10000);
            row->AddFrame(regTLowS[p], new TGLayoutHints(kLHintsLeft, 10, 2, 1, 1));
            row->AddFrame(new TGLabel(row, "-"),
                new TGLayoutHints(kLHintsLeft | kLHintsCenterY, 3, 3, 2, 2));
            regTHighS[p] = new TGNumberEntry(row, 0, 6, -1, TGNumberFormat::kNESInteger,
                TGNumberFormat::kNEANonNegative, TGNumberFormat::kNELLimitMinMax, 0, 10000);
            row->AddFrame(regTHighS[p], new TGLayoutHints(kLHintsLeft, 2, 2, 1, 1));

            // ch_end
            regChEnd[p] = new TGNumberEntry(row, 0, 6, -1, TGNumberFormat::kNESInteger,
                TGNumberFormat::kNEANonNegative, TGNumberFormat::kNELLimitMinMax, 0, 15359);
            row->AddFrame(regChEnd[p], new TGLayoutHints(kLHintsLeft, 10, 2, 1, 1));

            // t_low_e - t_high_e
            regTLowE[p] = new TGNumberEntry(row, 0, 6, -1, TGNumberFormat::kNESInteger,
                TGNumberFormat::kNEANonNegative, TGNumberFormat::kNELLimitMinMax, 0, 10000);
            row->AddFrame(regTLowE[p], new TGLayoutHints(kLHintsLeft, 10, 2, 1, 1));
            row->AddFrame(new TGLabel(row, "-"),
                new TGLayoutHints(kLHintsLeft | kLHintsCenterY, 3, 3, 2, 2));
            regTHighE[p] = new TGNumberEntry(row, 0, 6, -1, TGNumberFormat::kNESInteger,
                TGNumberFormat::kNEANonNegative, TGNumberFormat::kNELLimitMinMax, 0, 10000);
            row->AddFrame(regTHighE[p], new TGLayoutHints(kLHintsLeft, 2, 2, 1, 1));
        }

        // Button row
        {
            TGHorizontalFrame* btnRow = new TGHorizontalFrame(ctrl);
            ctrl->AddFrame(btnRow, new TGLayoutHints(kLHintsTop | kLHintsCenterX, 5, 5, 5, 5));

            TGTextButton* b;
            b = new TGTextButton(btnRow, "Set Start");
            b->Connect("Clicked()", "GuiController", this, "SetStartMode()");
            btnRow->AddFrame(b, new TGLayoutHints(kLHintsLeft, 5, 5, 2, 2));

            b = new TGTextButton(btnRow, "Set End");
            b->Connect("Clicked()", "GuiController", this, "SetEndMode()");
            btnRow->AddFrame(b, new TGLayoutHints(kLHintsLeft, 5, 5, 2, 2));

            b = new TGTextButton(btnRow, "Sum");
            b->Connect("Clicked()", "GuiController", this, "SumRegion()");
            btnRow->AddFrame(b, new TGLayoutHints(kLHintsLeft, 5, 5, 2, 2));

            b = new TGTextButton(btnRow, "Draw");
            b->Connect("Clicked()", "GuiController", this, "DrawRegion()");
            btnRow->AddFrame(b, new TGLayoutHints(kLHintsLeft, 5, 5, 2, 2));

            b = new TGTextButton(btnRow, "Erase");
            b->Connect("Clicked()", "GuiController", this, "EraseRegion()");
            btnRow->AddFrame(b, new TGLayoutHints(kLHintsLeft, 5, 5, 2, 2));

            b = new TGTextButton(btnRow, "Clear");
            b->Connect("Clicked()", "GuiController", this, "ClearRegion()");
            btnRow->AddFrame(b, new TGLayoutHints(kLHintsLeft, 5, 5, 2, 2));
        }

        // ---- Embedded canvas for histogram display ----
        regionCanvas = new TRootEmbeddedCanvas("regionCanvas", regionWindow, 690, 300);
        regionWindow->AddFrame(regionCanvas,
            new TGLayoutHints(kLHintsTop | kLHintsExpandX | kLHintsExpandY, 5, 5, 2, 5));

        regionWindow->MapSubwindows();
        regionWindow->Resize(regionWindow->GetDefaultSize());
        regionWindow->MapWindow();
    } else {
        regionWindow->RaiseWindow();
        regionWindow->MapWindow();
    }
}

void GuiController::HideRegionWindow()
{
    if (regionWindow) regionWindow->UnmapWindow();
}

void GuiController::SetStartMode()
{
    if (!regionWindow) ShowRegionWindow();
    captureMode = (captureMode == CAPTURE_START) ? CAPTURE_NONE : CAPTURE_START;
    cout << "Region sum: " << (captureMode == CAPTURE_START
        ? "click on a decon pad to set start channel/tick (click Set Start again to exit)"
        : "Set Start mode off") << endl;
}

void GuiController::SetEndMode()
{
    if (!regionWindow) ShowRegionWindow();
    captureMode = (captureMode == CAPTURE_END) ? CAPTURE_NONE : CAPTURE_END;
    cout << "Region sum: " << (captureMode == CAPTURE_END
        ? "click on a decon pad to set end channel/tick (click Set End again to exit)"
        : "Set End mode off") << endl;
}

void GuiController::SumRegion()
{
    if (!regionWindow || !regionCanvas) {
        cout << "SumRegion: open Region Sum window first" << endl;
        return;
    }

    // Per-plane time ranges
    double tLowS[3], tHighS[3], tLowE[3], tHighE[3];
    for (int p = 0; p < 3; ++p) {
        tLowS[p]  = regTLowS[p]->GetNumber();
        tHighS[p] = regTHighS[p]->GetNumber();
        tLowE[p]  = regTLowE[p]->GetNumber();
        tHighE[p] = regTHighE[p]->GetNumber();
    }

    // Global tick axis range: union over all planes
    double tMin = tLowS[0], tMax = tHighS[0];
    for (int p = 0; p < 3; ++p) {
        tMin = std::min({tMin, tLowS[p], tHighS[p], tLowE[p], tHighE[p]});
        tMax = std::max({tMax, tLowS[p], tHighS[p], tLowE[p], tHighE[p]});
    }
    if (tMax <= tMin) {
        cout << "SumRegion: time range is empty -- set t start/end values first" << endl;
        return;
    }

    static const char* planeLetter[3] = {"U", "V", "W"};
    static const Color_t planeColor[3] = {kRed, kBlue, kGreen+2};

    TCanvas* sc = regionCanvas->GetCanvas();
    sc->cd();
    sc->Clear();

    TH2F* hRef = data->wfs.at(3)->hOrig;
    int nTDCs  = hRef->GetNbinsY();
    double yAxisLow  = hRef->GetYaxis()->GetBinLowEdge(1);
    double yAxisHigh = hRef->GetYaxis()->GetBinUpEdge(nTDCs);

    TH1F* sumHist[3] = {};
    for (int p = 0; p < 3; ++p) {
        Waveforms* w = data->wfs.at(p + 3);
        TH2F* h = w->hOrig;

        double csVal = regChStart[p]->GetNumber();
        double ceVal = regChEnd[p]->GetNumber();
        if (csVal > ceVal) std::swap(csVal, ceVal);

        int csBin = h->GetXaxis()->FindBin(csVal);
        int ceBin = h->GetXaxis()->FindBin(ceVal);
        if (csBin > ceBin) std::swap(csBin, ceBin);

        TString sname = TString::Format("hRegionSum_%s", planeLetter[p]);
        TH1F* hExist = (TH1F*)gDirectory->FindObject(sname);
        if (hExist) delete hExist;

        TH1F* sum = new TH1F(sname, sname, nTDCs, yAxisLow, yAxisHigh);

        for (int i = csBin; i <= ceBin; ++i) {
            double frac = (ceBin == csBin) ? 0.0
                : (double)(i - csBin) / (double)(ceBin - csBin);
            double tl = tLowS[p]  + frac * (tLowE[p]  - tLowS[p]);
            double th = tHighS[p] + frac * (tHighE[p] - tHighS[p]);
            if (tl > th) std::swap(tl, th);
            int jLow  = h->GetYaxis()->FindBin(tl);
            int jHigh = h->GetYaxis()->FindBin(th);
            for (int j = jLow; j <= jHigh; ++j) {
                sum->AddBinContent(j, h->GetBinContent(i, j) * w->fScale);
            }
        }
        sumHist[p] = sum;
        cout << "SumRegion " << planeLetter[p] << ": ch "
             << (int)csVal << "-" << (int)ceVal
             << "  t[" << tLowS[p] << "-" << tHighS[p] << " -> "
             << tLowE[p] << "-" << tHighE[p] << "]"
             << "  integral=" << sum->Integral() << endl;
    }

    // Shared y range across all planes
    double yHi = -1e30, yLo = 1e30;
    for (int p = 0; p < 3; ++p) {
        if (sumHist[p]->GetMaximum() > yHi) yHi = sumHist[p]->GetMaximum();
        if (sumHist[p]->GetMinimum() < yLo) yLo = sumHist[p]->GetMinimum();
    }
    double yPad = (yHi > yLo) ? 0.1 * (yHi - yLo) : 1.0;

    TLegend* leg = new TLegend(0.75, 0.7, 0.95, 0.9);
    for (int p = 0; p < 3; ++p) {
        sumHist[p]->SetLineColor(planeColor[p]);
        sumHist[p]->SetLineWidth(2);
        sumHist[p]->GetXaxis()->SetRangeUser(tMin, tMax);
        sumHist[p]->GetXaxis()->SetTitle("ticks");
        sumHist[p]->GetYaxis()->SetTitle("summed signal");
        if (yHi > yLo)
            sumHist[p]->GetYaxis()->SetRangeUser(yLo - yPad, yHi + yPad);
        sumHist[p]->Draw(p == 0 ? "hist" : "hist same");

        TString entry = TString::Format("%s ch %d-%d", planeLetter[p],
            (int)regChStart[p]->GetNumber(),
            (int)regChEnd[p]->GetNumber());
        leg->AddEntry(sumHist[p], entry, "l");
    }
    leg->Draw();
    sc->Update();
}

void GuiController::DrawRegion()
{
    EraseRegion();  // remove any previous drawing first
    for (int p = 0; p < 3; ++p) {
        if (!regChStart[p]) continue;
        double cs  = regChStart[p]->GetNumber();
        double ce  = regChEnd[p]->GetNumber();
        double tls = regTLowS[p]->GetNumber();
        double ths = regTHighS[p]->GetNumber();
        double tle = regTLowE[p]->GetNumber();
        double the = regTHighE[p]->GetNumber();

        int padNo = p + 4;  // pads 4/5/6 = U/V/W decon
        vw->can->cd(padNo);

        // edge 0: left side (at ch_start), t_low_s to t_high_s
        regionBoundary[p][0] = new TLine(cs, tls, cs, ths);
        // edge 1: right side (at ch_end), t_low_e to t_high_e
        regionBoundary[p][1] = new TLine(ce, tle, ce, the);
        // edge 2: top — t_high_s to t_high_e
        regionBoundary[p][2] = new TLine(cs, ths, ce, the);
        // edge 3: bottom — t_low_s to t_low_e
        regionBoundary[p][3] = new TLine(cs, tls, ce, tle);

        for (int e = 0; e < 4; ++e) {
            regionBoundary[p][e]->SetLineColor(kOrange+7);
            regionBoundary[p][e]->SetLineWidth(2);
            regionBoundary[p][e]->SetLineStyle(2);  // dashed
            regionBoundary[p][e]->Draw();
        }
        vw->can->GetPad(padNo)->Modified();
        vw->can->GetPad(padNo)->Update();
    }
}

void GuiController::EraseRegion()
{
    for (int p = 0; p < 3; ++p) {
        int padNo = p + 4;
        TVirtualPad* pad = vw->can->GetPad(padNo);
        bool changed = false;
        for (int e = 0; e < 4; ++e) {
            if (regionBoundary[p][e]) {
                pad->GetListOfPrimitives()->Remove(regionBoundary[p][e]);
                delete regionBoundary[p][e];
                regionBoundary[p][e] = nullptr;
                changed = true;
            }
        }
        if (changed) {
            pad->Modified();
            pad->Update();
        }
    }
}

void GuiController::ClearRegion()
{
    EraseRegion();
    for (int p = 0; p < 3; ++p) {
        if (regChStart[p])  regChStart[p]->SetNumber(0);
        if (regChEnd[p])    regChEnd[p]->SetNumber(0);
        if (regTLowS[p])    regTLowS[p]->SetNumber(0);
        if (regTHighS[p])   regTHighS[p]->SetNumber(0);
        if (regTLowE[p])    regTLowE[p]->SetNumber(0);
        if (regTHighE[p])   regTHighE[p]->SetNumber(0);
    }
    captureMode = CAPTURE_NONE;
    if (regionCanvas) {
        TCanvas* sc = regionCanvas->GetCanvas();
        sc->Clear();
        sc->Update();
    }
}

void GuiController::HandleMenu(int id)
{
    // const char *filetypes[] = {"ROOT files", "*.root", 0, 0};
    switch (id) {
        case M_FILE_EXIT:
            gApplication->Terminate(0);
            break;
    }
}

TString GuiController::OpenDialog()
{
    const char *filetypes[] = {"ROOT files", "*.root", 0, 0};
    TString currentDir(gSystem->WorkingDirectory());
    static TString dir(".");
    TGFileInfo fi;
    fi.fFileTypes = filetypes;
    fi.fIniDir    = StrDup(dir);
    new TGFileDialog(gClient->GetRoot(), mw, kFDOpen, &fi);
    dir = fi.fIniDir;
    gSystem->cd(currentDir.Data());

    if (fi.fFilename) {
        // UnZoom();
        cout << "open file: " << fi.fFilename << endl;
        return fi.fFilename;
    }
    else {
        gApplication->Terminate(0);
    }
    return "";

}
