#include "Data.h"
#include "Waveforms.h"
#include "RawWaveforms.h"
#include "BadChannels.h"

#include "TH2F.h"
#include "TH2I.h"
#include "TH1I.h"
#include "TFile.h"
#include "TTree.h"
#include "TString.h"
#include "TSystem.h"

#include <vector>
#include <string>
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <fstream>

using namespace std;


Data::Data()
{}

Data::Data(const char* filename, double threshold, const char* frame, int rebin)
{
    rootFile = TFile::Open(filename);
    if (!rootFile) {
    	string msg = "Unable to open ";
    	msg += filename;
    	throw runtime_error(msg.c_str());
    }

    // load_runinfo first so anodeNo is available for all histogram name lookups
    load_runinfo();

    // suffix is the anode number when present (e.g. "7"), otherwise empty string
    TString suf = (anodeNo >= 0) ? Form("%d", anodeNo) : "";
    if (anodeNo >= 0)
        cout << "Anode number: " << anodeNo << " (using suffix \"" << suf << "\")" << endl;

    bad_channels = new BadChannels( (TTree*)rootFile->Get("T_bad" + suf) );

    load_channelstatus();

    // denoised (post noise-filter) — no fallback, just load with suffix
    load_waveform("hu_raw" + suf, "U Plane (Denoised)", 1., threshold);
    load_waveform("hv_raw" + suf, "V Plane (Denoised)", 1., threshold);
    load_waveform("hw_raw" + suf, "W Plane (Denoised)", 1., threshold);

    // deconvoluted — try requested frame, fall back to _gauss
    for (int iplane=0; iplane<3; ++iplane) {
        TString histName = Form("h%c_%s", 'u'+iplane, frame) + suf;
        if (!rootFile->Get(histName)) {
            TString fallback = Form("h%c_gauss", 'u'+iplane) + suf;
            if (rootFile->Get(fallback)) {
                cout << histName << " not found, falling back to " << fallback << endl;
                histName = fallback;
            }
        }
        load_waveform(histName, Form("%c Plane (Deconvoluted)", 'U'+iplane), 1./(500.*rebin/4.0), threshold);
    }

    load_rawwaveform("hu_orig" + suf, "hu_baseline" + suf);
    load_rawwaveform("hv_orig" + suf, "hv_baseline" + suf);
    load_rawwaveform("hw_orig" + suf, "hw_baseline" + suf);

    load_threshold("hu_threshold" + suf);
    load_threshold("hv_threshold" + suf);
    load_threshold("hw_threshold" + suf);
}

void Data::load_runinfo()
{
    anodeNo = -1;
    total_time_bin = 0;
    TTree *t = (TTree*)rootFile->Get("Trun");
    if (t) {
        t->SetBranchAddress("runNo", &runNo);
        t->SetBranchAddress("subRunNo", &subRunNo);
        t->SetBranchAddress("eventNo", &eventNo);
        if (t->GetBranch("anodeNo"))       t->SetBranchAddress("anodeNo", &anodeNo);
        if (t->GetBranch("total_time_bin")) t->SetBranchAddress("total_time_bin", &total_time_bin);
        t->GetEntry(0);
    }
    else {
        runNo = 0;
        subRunNo = 0;
        eventNo = 0;
    }
}

void Data::load_channelstatus(){
    string currentDir(gSystem->WorkingDirectory());

    std::ifstream in(currentDir + "/../data/badchan.txt");
    std::string input;
    while(std::getline(in, input)){
        std::stringstream stream(input);
        int nchan;
        stream >> nchan;
        std::string description = stream.str();
        size_t ind = description.find_first_of("#");
        description = description.substr(ind+1);
        channel_status[nchan] = "(Bad) " + description;
    }
    in.close();

    in.open(currentDir + "/../data/noisychan.txt");
    while(std::getline(in, input)){
        std::stringstream stream(input);
        int nchan;
        stream >> nchan;
        std::string description = stream.str();
        size_t ind = description.find_first_of("#");
        description = description.substr(ind+1);
        channel_status[nchan] = "(Noisy)" + description;
    }
    in.close();
}

int Data::GetPlaneNo(int chanNo)
{
    // 800 u + 800 v + 960 w = 2560
    int apaNo = chanNo / 2560;
    int offset = chanNo - apaNo*2560;
    if (offset < 800) {
        return 0;
    }
    else if (offset < 1600) {
        return 1;
    }
    else {
        return 2;
    }
}

// Wrap up some ROOT pointer dancing.
template<class NEED, class WANT>
WANT* maybe_cast(TObject* obj, const std::vector<std::string>& okay_types, bool throw_on_err=false)
{
    NEED* base = dynamic_cast<NEED*>(obj);
    if (!base) {
	return nullptr;
    }
    bool ok = false;
    for (auto type_name : okay_types) {
	if (base->InheritsFrom(type_name.c_str())) {
	    ok = true;
	    break;
	}
    }
    if (ok) {
	return static_cast<WANT*>(base);
    }
    if (throw_on_err) {
	stringstream ss;
	ss << "TObject not one of type: [";
	string comma = "";
	for (auto type_name : okay_types) {
	    ss << comma << type_name;
	    comma = ", ";
	}
        throw runtime_error(ss.str().c_str());
    }
    return nullptr;
}

void Data::load_waveform(const char* name, const char* title, double scale, double threshold)
{
    TObject* obj = rootFile->Get(name);
    if (!obj) {
    	TString msg = "Failed to get waveform ";
    	msg += name;
        msg += ", create dummy ...";
        cout << msg << endl;
    	// throw runtime_error(msg.c_str());
        int nChannels = 2400;
        int nTDCs = 6000;
        int firstChannel = 0;
        if (msg.Contains("hv")) {
            firstChannel = 2400;
        }
        else if (msg.Contains("hw")) {
            firstChannel = 4800;
            nChannels = 3456;
        }
        obj = new TH2F(name, title, nChannels,firstChannel-0.5,firstChannel+nChannels-0.5,nTDCs,0,nTDCs);
    }
    auto hist = maybe_cast<TH2, TH2F>(obj, {"TH2F", "TH2I"}, true);
    hist->SetXTitle("channel");
    hist->SetYTitle("ticks");
    wfs.push_back( new Waveforms(hist, bad_channels, name, title, scale, threshold) );
}

void Data::load_rawwaveform(const char* name, const char* baseline_name)
{
    TObject* obj = rootFile->Get(name);
    if (!obj) {
        TString msg = "Failed to get waveform ";
        msg += name;
        msg += ", create dummy ...";
        cout << msg << endl;
        // throw runtime_error(msg.c_str());
        int nChannels = 2400;
        int nTDCs = 6000;
        int firstChannel = 0;
        if (msg.Contains("hv")) {
            firstChannel = 2400;
        }
        else if (msg.Contains("hw")) {
            firstChannel = 4800;
            nChannels = 3456;
        }
        obj = new TH2I(name, "", nChannels,firstChannel-0.5,firstChannel+nChannels-0.5,nTDCs,0,nTDCs);
    }

    auto hist = maybe_cast<TH2, TH2I>(obj, {"TH2I", "TH2F"}, true);
    hist->SetXTitle("channel");
    hist->SetYTitle("ticks");

    TObject* obj2 = rootFile->Get(baseline_name);
    if (!obj2) {
        // string msg = "Failed to get baseline ";
        // msg += baseline_name;
        // msg += ", create dummy ...";
        // // throw runtime_error(msg.c_str());
        // obj2 = new TH1I(baseline_name, "", hist->GetNbinsX(),0,hist->GetNbinsX());
        raw_wfs.push_back( new RawWaveforms(hist, 0) );
    }
    else {
        TH1I* hist2 = dynamic_cast<TH1I*>(obj2);
        if (!hist2) {
            string msg = "Not a TH1I: ";
            msg += name;
            throw runtime_error(msg.c_str());
        }
        raw_wfs.push_back( new RawWaveforms(hist, hist2) );
    }

}

void Data::load_threshold(const char* name)
{
    TObject* obj = rootFile->Get(name);
    if (!obj) {
        string msg = "Failed to get threshold ";
        msg += name;
        msg += ", create dummy ...";
        // throw runtime_error(msg.c_str());
        obj = new TH1I(name, "", 4000,0,4000);
    }
    auto hist = maybe_cast<TH1, TH1I>(obj, {"TH1I", "TH1F"}, true);
    thresh_histos.push_back( hist );
}

Data::~Data()
{
    delete rootFile;
}
// Local Variables:
// mode: c++
// c-basic-offset: 4
// End:
