# QCustomPlot 2.1.1

Unmodified upstream source from https://www.qcustomplot.com/release/2.1.1/QCustomPlot-source.tar.gz
Copyright Emanuel Eichhammer. License: GPL-3.0-or-later, see GPL.txt and the source headers.

The same 2.1.1 version is present in the local EMBO project; the RespStudio HostGUI's
`app_wnd_response::initCommonPlot` was consulted for graph setup, legend, and range interactions.
`AnalysisPlot` adapts QCustomPlot for Qt Quick: QWidget work stays on the GUI thread,
and the scene graph only paints a cached image. The host uses QApplication for Widgets support.
