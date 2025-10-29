#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>

#include "GUI/MainWindow.h"
#include "GUI/Settings/SConfig.h"

#include "version.h"

int main(int argc, char** argv)
{
  QApplication app(argc, argv);
  QApplication::setApplicationName("Flycast Memory Engine");
  QApplication::setApplicationVersion(APP_VERSION);

  SConfig config;  // Initialize global settings object

  QCommandLineParser parser;
  parser.setApplicationDescription(
      QObject::tr("A RAM search derived from Flycast Memory Engine to search, monitor and edit "
                  "the Flycast Emulator's emulated memory. Heavily based off of Dolphin Memory Engine."));
  parser.addHelpOption();
  parser.addVersionOption();

  const QCommandLineOption FlycastProcessNameOption(
      QStringList() << "d" << "flycast-process-name",
      QObject::tr("Specify custom name for the Flycast Emulator process. By default, "
                  "platform-specific names are used (e.g. \"flycast.exe\" on Windows, or "
                  "\"flycast-emu\"? on Linux or macOS). Check Task Manager or btop if in doubt."),
      "flycast_process_name");
  parser.addOption(FlycastProcessNameOption);

  parser.process(app);

  const QString FlycastProcessName{parser.value(FlycastProcessNameOption)};
  if (!FlycastProcessName.isEmpty())
  {
    qputenv("FME_FLYCAST_PROCESS_NAME", FlycastProcessName.toStdString().c_str());
  }

  MainWindow window;

  if (!config.ownsSettingsFile())
  {
    QMessageBox box(
        QMessageBox::Warning, QObject::tr("Another instance is already running"),
        QObject::tr(
            "Changes made to settings will not be preserved in this session. This includes changes "
            "to the watch list, which will need to be saved manually into a file."),
        QMessageBox::Ok);
    box.setWindowIcon(window.windowIcon());
    box.exec();
  }

  window.show();
  return QApplication::exec();
}
