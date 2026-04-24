#include "RawWaveforms.h"

#include "TH2I.h"
#include "TH1I.h"
#include "TColor.h"
#include "TLine.h"
#include "TDirectory.h"
#include "TString.h"
#include "TPad.h"
#include "TMath.h"

#include <algorithm>
#include <iostream>
#include <vector>
using namespace std;

RawWaveforms::RawWaveforms()
{}

RawWaveforms::RawWaveforms(TH2I *h2, TH1I *h)
{
    // 2D hist: x: channel id; y: tdc 0 - 9600 or 0 - 9600/4
    hOrig = h2;
    fName = hOrig->GetName();
    hBaseline = h;
    nChannels = hOrig->GetNbinsX();
    nTDCs = hOrig->GetNbinsY();
    firstChannel = hOrig->GetXaxis()->GetBinCenter(1);
    SetBaseline();
    // cout << nChannels << ", " << nTDCs << endl;
}

RawWaveforms::~RawWaveforms()
{
}

void RawWaveforms::SetBaseline()
{
    // Compute per-channel median pedestal from hOrig, store it in hBaseline,
    // and subtract it from the TH2I in-place so downstream code sees
    // baseline-free ADC for both 2D display and 1D waveform extraction.
    if (!hBaseline) {
        hBaseline = new TH1I(fName+"_baseline", "",
            nChannels, -0.5+firstChannel, -0.5+firstChannel+nChannels);
    }
    cout << "calculating baseline (median) for " << fName << " ..." << endl;
    vector<int> ticks(nTDCs);
    for (int chid = 0; chid < nChannels; ++chid) {
        for (int ibin = 0; ibin < nTDCs; ++ibin)
            ticks[ibin] = int(hOrig->GetBinContent(chid+1, ibin+1));
        nth_element(ticks.begin(), ticks.begin() + nTDCs/2, ticks.end());
        int median = ticks[nTDCs/2];
        hBaseline->SetBinContent(chid+1, median);
        for (int ibin = 0; ibin < nTDCs; ++ibin)
            hOrig->SetBinContent(chid+1, ibin+1,
                hOrig->GetBinContent(chid+1, ibin+1) - median);
    }
}

TH1I* RawWaveforms::Draw1D(int chanNo, const char* options)
{
    TString name = TString::Format("hWire_%s", fName.Data());

    TH1I *hWire = (TH1I*)gDirectory->FindObject(name);
    if (hWire) delete hWire;

    hWire = new TH1I(name, "",
        nTDCs,
        hOrig->GetYaxis()->GetBinLowEdge(0),
        hOrig->GetYaxis()->GetBinUpEdge(nTDCs)
    );
    int binNo = hOrig->GetXaxis()->FindBin(chanNo);
    int baseline = hBaseline->GetBinContent(binNo);  // kept for title/ref-lines only
    for (int i=1; i<=nTDCs; i++) {
        hWire->SetBinContent(i, hOrig->GetBinContent(binNo, i));
    }
    hWire->SetTitle( TString::Format("baseline @%i", baseline) );
    hWire->Draw(options);

    for (int i=0; i<64; i++) {
        if ((baseline-i)%64 == 0) {
            TLine *line = new TLine(0, -i, nTDCs, -i);
            line->SetLineColorAlpha(kRed-3, 0.5);
            line->Draw();
        }
    }

    return hWire;
}
