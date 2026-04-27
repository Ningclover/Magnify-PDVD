// Fix duplicate LC_RPATH entries produced by ACLiC on macOS 15+ with conda ROOT.
// Without this, dlopen refuses to load the .so and ROOT segfaults.
static void fixRpath(const char* soPath) {
    TString cmd = TString::Format(
        "n=$(otool -l '%s' 2>/dev/null | grep -c LC_RPATH || echo 0);"
        "if [ \"$n\" -gt 1 ]; then"
        "  install_name_tool -delete_rpath /opt/anaconda3/envs/evn2/lib '%s' 2>/dev/null;"
        "  install_name_tool -add_rpath    /opt/anaconda3/envs/evn2/lib '%s' 2>/dev/null;"
        "fi",
        soPath, soPath, soPath);
    gSystem->Exec(cmd);
}

static void loadAndFix(const char* src) {
    // Derive .so path from source path (ACLiC convention: foo.cc → foo_cc.so)
    TString so(src);
    so.ReplaceAll(".cc", "_cc.so");
    gROOT->ProcessLine(TString(".L ") + src + "+");
    fixRpath(so);
    // Reload after RPATH fix so ROOT actually has the symbols
    if (gSystem->Load(so) < 0)
        fprintf(stderr, "WARNING: failed to load %s after RPATH fix\n", so.Data());
}

void loadClasses()
{
    TString include = ".include ";
    TString pwd = gSystem->pwd();
    pwd = pwd + '/';

    TString prefix;
    prefix = "../event";
    gROOT->ProcessLine( include + pwd + prefix );
    loadAndFix( prefix + "/BadChannels.cc" );
    loadAndFix( prefix + "/RawWaveforms.cc" );
    loadAndFix( prefix + "/Waveforms.cc" );
    loadAndFix( prefix + "/Data.cc" );

    prefix = "../viewer";
    gROOT->ProcessLine( include + pwd + prefix );
    loadAndFix( prefix + "/RmsAnalyzer.cc" );
    loadAndFix( prefix + "/ViewWindow.cc" );
    loadAndFix( prefix + "/ControlWindow.cc" );
    loadAndFix( prefix + "/MainWindow.cc" );
    loadAndFix( prefix + "/GuiController.cc" );
}
