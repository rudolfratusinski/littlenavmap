/*****************************************************************************
* Copyright 2015-2020 Alexander Barthel alex@littlenavmap.org
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "weather/windreporter.h"

#include "navapp.h"
#include "ui_mainwindow.h"
#include "grib/windquery.h"
#include "settings/settings.h"
#include "common/constants.h"
#include "options/optiondata.h"
#include "common/unit.h"
#include "perf/aircraftperfcontroller.h"
#include "route/route.h"
#include "mapgui/maplayer.h"
#include "gui/dialog.h"

#include <QToolButton>
#include <QDebug>
#include <QMessageBox>
#include <QDir>

static double queryRectInflationFactor = 0.2;
static double queryRectInflationIncrement = 0.1;
static int queryMaxRows = 5000;

WindReporter::WindReporter(QObject *parent, atools::fs::FsPaths::SimulatorType type)
  : QObject(parent), simType(type)
{
  atools::settings::Settings& settings = atools::settings::Settings::instance();
  verbose = settings.getAndStoreValue(lnm::OPTIONS_WEATHER_DEBUG, false).toBool();
  if(settings.contains(lnm::OPTIONS_WEATHER_LEVELS))
  {
    QStringList strlevels = settings.valueStrList(lnm::OPTIONS_WEATHER_LEVELS);
    levels.clear();
    levelsTooltip.clear();
    for(const QString& str : strlevels)
    {
      bool ok;
      int num = str.toInt(&ok);
      if(ok && num > 0)
        levels.append(num);
      levelsTooltip.append(num);
    }
  }

  // Real wind ==================
  windQueryOnline = new atools::grib::WindQuery(parent, verbose);
  connect(windQueryOnline, &atools::grib::WindQuery::windDataUpdated, this, &WindReporter::windDownloadFinished);
  connect(windQueryOnline, &atools::grib::WindQuery::windDownloadFailed, this, &WindReporter::windDownloadFailed);
  connect(windQueryOnline, &atools::grib::WindQuery::windDownloadSslErrors, this, &WindReporter::windDownloadSslErrors);
  connect(windQueryOnline, &atools::grib::WindQuery::windDownloadProgress, this, &WindReporter::windDownloadProgress);

  // Layers from custom settings ==================
  windQueryManual = new atools::grib::WindQuery(parent, verbose);
  windQueryManual->initFromFixedModel(0.f, 0.f, 0.f);

  Ui::MainWindow *ui = NavApp::getMainUi();
  connect(ui->actionMapShowWindDisabled, &QAction::triggered, this, &WindReporter::sourceActionTriggered);
  connect(ui->actionMapShowWindNOAA, &QAction::triggered, this, &WindReporter::sourceActionTriggered);
  connect(ui->actionMapShowWindSimulator, &QAction::triggered, this, &WindReporter::sourceActionTriggered);
}

WindReporter::~WindReporter()
{
  delete windQueryOnline;
  delete windQueryManual;
  delete actionGroup;
  delete windlevelToolButton;
}

void WindReporter::preDatabaseLoad()
{

}

void WindReporter::postDatabaseLoad(atools::fs::FsPaths::SimulatorType type)
{
  if(type != simType)
  {
    // Simulator has changed - reload files
    simType = type;
    updateDataSource();
  }
}

void WindReporter::optionsChanged()
{
  updateDataSource();
}

void WindReporter::saveState()
{
  atools::settings::Settings::instance().setValue(lnm::MAP_WIND_LEVEL, currentLevel);
  atools::settings::Settings::instance().setValue(lnm::MAP_WIND_LEVEL_ROUTE, showFlightplanWaypoints);
  atools::settings::Settings::instance().setValue(lnm::MAP_WIND_SOURCE, currentSource);
}

void WindReporter::restoreState()
{
  if(OptionData::instance().getFlags() & opts::STARTUP_LOAD_MAP_SETTINGS)
  {
    atools::settings::Settings& settings = atools::settings::Settings::instance();
    currentLevel = settings.valueInt(lnm::MAP_WIND_LEVEL, wind::NONE);
    showFlightplanWaypoints = settings.valueBool(lnm::MAP_WIND_LEVEL_ROUTE, false);
    currentSource = static_cast<wind::WindSource>(settings.valueInt(lnm::MAP_WIND_SOURCE, wind::NOAA));
  }
  valuesToAction();

  // Download wind data with a delay after startup
  QTimer::singleShot(2000, this, &WindReporter::updateDataSource);
  updateToolButtonState();
}

void WindReporter::updateDataSource()
{
  Ui::MainWindow *ui = NavApp::getMainUi();

  actionToValues();
  downloadErrorReported = false;

  if(ui->actionMapShowWindSimulator->isChecked() && simType == atools::fs::FsPaths::XPLANE11)
  {
    // Load GRIB file only if X-Plane is enabled - will call windDownloadFinished later
    QString path = OptionData::instance().getWeatherXplaneWind();
    if(path.isEmpty())
      path = NavApp::getSimulatorBasePath(NavApp::getCurrentSimulatorDb()) + QDir::separator() + "global_winds.grib";

    if(QFileInfo::exists(path))
      windQueryOnline->initFromFile(path);
  }
  else if(ui->actionMapShowWindNOAA->isChecked())
    // Download from NOAA - will call windDownloadFinished later
    windQueryOnline->initFromUrl(OptionData::instance().getWeatherNoaaWindBaseUrl());
  else
  {
    windQueryOnline->deinit();
    updateToolButtonState();
    emit windUpdated();
  }
}

void WindReporter::windDownloadFinished()
{
  qDebug() << Q_FUNC_INFO;
  updateToolButtonState();
  if(!isWindManual())
  {
    QDateTime from, to;
    windQueryOnline->getValidity(from, to);

    QString validText = from.isValid() && to.isValid() ? tr(" Forecast from %1 to %2 UTC.").
                        arg(QLocale().toString(from, QLocale::ShortFormat)).
                        arg(QLocale().toString(to, QLocale::ShortFormat)) : QString();

    QString msg;
    switch(currentSource)
    {
      case wind::NO_SOURCE:
        break;

      case wind::SIMULATOR:
        msg = tr("Winds aloft updated from simulator.%1").arg(validText);
        break;

      case wind::NOAA:
        msg = tr("Winds aloft downloaded from NOAA.%1").arg(validText);
        break;
    }
    NavApp::setStatusMessage(msg, true /* addToLog */);
  }
  emit windUpdated();
}

void WindReporter::windDownloadProgress(qint64 bytesReceived, qint64 bytesTotal, QString downloadUrl)
{
  if(verbose)
    qDebug() << Q_FUNC_INFO << "bytesReceived" << bytesReceived << "bytesTotal" << bytesTotal
             << "downloadUrl" << downloadUrl;

  QApplication::processEvents(QEventLoop::WaitForMoreEvents);
}

void WindReporter::windDownloadSslErrors(const QStringList& errors, const QString& downloadUrl)
{
  int result = atools::gui::Dialog(NavApp::getQMainWindow()).
               showQuestionMsgBox(lnm::ACTIONS_SHOW_SSL_WARNING_WIND,
                                  tr("<p>Errors while trying to establish an encrypted "
                                       "connection to download winds aloft:</p>"
                                       "<p>URL: %1</p>"
                                         "<p>Error messages:<br/>%2</p>"
                                           "<p>Continue?</p>").
                                  arg(downloadUrl).
                                  arg(atools::strJoin(errors, tr("<br/>"))),
                                  tr("Do not &show this again and ignore errors in the future"),
                                  QMessageBox::Cancel | QMessageBox::Yes,
                                  QMessageBox::Cancel, QMessageBox::Yes);

  windQueryOnline->setIgnoreSslErrors(result == QMessageBox::Yes);
}

void WindReporter::windDownloadFailed(const QString& error, int errorCode)
{
  qDebug() << Q_FUNC_INFO << error << errorCode;

  if(!downloadErrorReported)
  {
    // Get rid of splash in case this happens on startup
    NavApp::deleteSplashScreen();
    QMessageBox::warning(NavApp::getQMainWidget(), QApplication::applicationName(),
                         tr("Error downloading or reading wind data: %1 (%2)").arg(error).arg(errorCode));
    downloadErrorReported = true;
  }
  updateToolButtonState();
}

void WindReporter::addToolbarButton()
{
  // Create and add toolbar button =====================================
  Ui::MainWindow *ui = NavApp::getMainUi();
  windlevelToolButton = new QToolButton(ui->toolbarMapOptions);
  windlevelToolButton->setIcon(QIcon(":/littlenavmap/resources/icons/wind.svg"));
  windlevelToolButton->setPopupMode(QToolButton::InstantPopup);
  windlevelToolButton->setToolTip(tr("Wind forecast altitude levels to display"));
  windlevelToolButton->setStatusTip(windlevelToolButton->toolTip());
  windlevelToolButton->setCheckable(true);

  // Add tear off menu to button =======
  windlevelToolButton->setMenu(new QMenu(windlevelToolButton));
  QMenu *buttonMenu = windlevelToolButton->menu();
  buttonMenu->setToolTipsVisible(true);
  buttonMenu->setTearOffEnabled(true);

  // Insert before show route
  ui->toolbarMapOptions->insertWidget(ui->actionMapShowSunShading, windlevelToolButton);
  ui->menuHighAltitudeWindLevels->clear();

  // Create and add flight plan action =====================================
  actionFlightplanWaypoints = new QAction(tr("&At Flight Plan Waypoints"), buttonMenu);
  actionFlightplanWaypoints->setToolTip(tr("Show wind at flight plan waypoints"));
  actionFlightplanWaypoints->setStatusTip(actionFlightplanWaypoints->toolTip());
  actionFlightplanWaypoints->setCheckable(true);
  buttonMenu->addAction(actionFlightplanWaypoints);
  ui->menuHighAltitudeWindLevels->addAction(actionFlightplanWaypoints);
  connect(actionFlightplanWaypoints, &QAction::triggered, this, &WindReporter::toolbarActionFlightplanTriggered);

  ui->menuHighAltitudeWindLevels->addSeparator();
  buttonMenu->addSeparator();

  actionGroup = new QActionGroup(buttonMenu);
  // Create and add none action =====================================
  actionNone = new QAction(tr("&None"), buttonMenu);
  actionNone->setToolTip(tr("Do not show wind barbs"));
  actionNone->setStatusTip(actionNone->toolTip());
  actionNone->setData(wind::NONE);
  actionNone->setCheckable(true);
  actionNone->setActionGroup(actionGroup);
  buttonMenu->addAction(actionNone);
  ui->menuHighAltitudeWindLevels->addAction(actionNone);
  connect(actionNone, &QAction::triggered, this, &WindReporter::toolbarActionTriggered);

  // Create and add level actions =====================================
  for(int level : levels)
  {
    if(level == wind::AGL)
    {
      // Create and add ground/AGL action =====================================
      actionAgl = new QAction(tr("Ground (only NOAA)"), buttonMenu);
      actionAgl->setToolTip(tr("Show wind for 80 m / 260 ft above ground"));
      actionAgl->setStatusTip(actionAgl->toolTip());
      actionAgl->setData(wind::AGL);
      actionAgl->setCheckable(true);
      actionAgl->setActionGroup(actionGroup);
      buttonMenu->addAction(actionAgl);
      ui->menuHighAltitudeWindLevels->addAction(actionAgl);
      connect(actionAgl, &QAction::triggered, this, &WindReporter::toolbarActionTriggered);
    }
    else if(level == wind::FLIGHTPLAN)
    {
      // Create and add flight plan action =====================================
      actionFlightplan = new QAction(tr("At Flight Plan Cruise Altitude"), buttonMenu);
      actionFlightplan->setToolTip(tr("Show wind at flight plan cruise altitude"));
      actionFlightplan->setStatusTip(actionFlightplan->toolTip());
      actionFlightplan->setData(wind::FLIGHTPLAN);
      actionFlightplan->setCheckable(true);
      actionFlightplan->setActionGroup(actionGroup);
      buttonMenu->addAction(actionFlightplan);
      ui->menuHighAltitudeWindLevels->addAction(actionFlightplan);
      connect(actionFlightplan, &QAction::triggered, this, &WindReporter::toolbarActionTriggered);
      ui->menuHighAltitudeWindLevels->addSeparator();
      buttonMenu->addSeparator();
    }
    else
    {
      QAction *action = new QAction(tr("At %1").arg(Unit::altFeet(level)), buttonMenu);
      action->setToolTip(tr("Show wind at %1 altitude").arg(Unit::altFeet(level)));
      action->setData(level);
      action->setCheckable(true);
      action->setStatusTip(action->toolTip());
      action->setActionGroup(actionGroup);
      buttonMenu->addAction(action);
      actionLevelVector.append(action);
      ui->menuHighAltitudeWindLevels->addAction(action);
      connect(action, &QAction::triggered, this, &WindReporter::toolbarActionTriggered);
    }
  }
}

bool WindReporter::isWindShown() const
{
  return currentLevel != wind::NONE;
}

bool WindReporter::isRouteWindShown() const
{
  return showFlightplanWaypoints;
}

bool WindReporter::hasOnlineWindData() const
{
  return windQueryOnline->hasWindData();
}

bool WindReporter::isWindManual() const
{
  return NavApp::getAircraftPerfController()->isWindManual();
}

void WindReporter::sourceActionTriggered()
{
  if(!ignoreUpdates)
  {
    updateDataSource();
    updateToolButtonState();
  }
}

void WindReporter::toolbarActionFlightplanTriggered()
{
  if(!ignoreUpdates)
  {
    actionToValues();
    updateToolButtonState();
    emit windUpdated();
  }
}

void WindReporter::toolbarActionTriggered()
{
  if(!ignoreUpdates)
  {
    actionToValues();
    updateToolButtonState();
    emit windUpdated();
  }
}

void WindReporter::updateToolButtonState()
{
  bool onlineWind = windQueryOnline->hasWindData();
  bool manualWind = isWindManual();

  // Either selection will show button depressed independent if enabled or not
  windlevelToolButton->setChecked(!actionNone->isChecked() || actionFlightplanWaypoints->isChecked());

  // Actions that need real wind or manual wind
  actionNone->setEnabled(onlineWind || manualWind);
  actionFlightplan->setEnabled(onlineWind || manualWind);
  actionAgl->setEnabled(onlineWind || manualWind);

  for(QAction *action: actionLevelVector)
    action->setEnabled(onlineWind || manualWind);

  actionFlightplanWaypoints->setEnabled(!NavApp::getRoute().isFlightplanEmpty() && (onlineWind || manualWind));
}

void WindReporter::valuesToAction()
{
  ignoreUpdates = true;
  switch(currentLevel)
  {
    case wind::NONE:
      actionNone->setChecked(true);
      break;

    case wind::AGL:
      actionAgl->setChecked(true);
      break;

    case wind::FLIGHTPLAN:
      actionFlightplan->setChecked(true);
      break;

    default:
      for(QAction *action : actionLevelVector)
      {
        if(action->data().toInt() == currentLevel)
          action->setChecked(true);
      }
  }

  actionFlightplanWaypoints->setChecked(showFlightplanWaypoints);

  qDebug() << Q_FUNC_INFO << "source" << currentSource;
  switch(currentSource)
  {
    case wind::NO_SOURCE:
      NavApp::getMainUi()->actionMapShowWindDisabled->setChecked(true);
      break;
    case wind::NOAA:
      NavApp::getMainUi()->actionMapShowWindNOAA->setChecked(true);
      break;
    case wind::SIMULATOR:
      NavApp::getMainUi()->actionMapShowWindSimulator->setChecked(true);
      break;
  }
  ignoreUpdates = false;
}

QString WindReporter::getLevelText() const
{
  switch(currentLevel)
  {
    case wind::NONE:
      return tr("None");

    case wind::AGL:
      return tr("Ground");

    case wind::FLIGHTPLAN:
      return tr("Flight plan cruise altitude");

    default:
      return Unit::altFeet(currentLevel);
  }
}

QString WindReporter::getSourceText() const
{
  if(isWindManual())
    return tr("Manual");

  switch(currentSource)
  {
    case wind::NO_SOURCE:
      return tr("Disabled");

    case wind::NOAA:
      return tr("NOAA");

    case wind::SIMULATOR:
      return tr("Simulator");
  }
  return QString();
}

void WindReporter::resetSettingsToDefault()
{
  currentLevel = wind::NONE;
  showFlightplanWaypoints = false;

  valuesToAction();
}

void WindReporter::actionToValues()
{
  QAction *action = actionGroup->checkedAction();
  if(action != nullptr)
    currentLevel = action->data().toInt();
  else
    currentLevel = wind::NONE;

  if(NavApp::getMainUi()->actionMapShowWindDisabled->isChecked())
    currentSource = wind::NO_SOURCE;
  else if(NavApp::getMainUi()->actionMapShowWindNOAA->isChecked())
    currentSource = wind::NOAA;
  else if(NavApp::getMainUi()->actionMapShowWindSimulator->isChecked())
    currentSource = wind::SIMULATOR;

  showFlightplanWaypoints = actionFlightplanWaypoints->isChecked();

  qDebug() << Q_FUNC_INFO << currentLevel;
}

float WindReporter::getAltitude() const
{
  switch(currentLevel)
  {
    case wind::NONE:
      return 0.f;

    case wind::AGL:
      return 260.f;

    case wind::FLIGHTPLAN:
      return NavApp::getRouteCruiseAltFt();

    default:
      if(actionGroup->checkedAction())
        return actionGroup->checkedAction()->data().toInt();
      else
        return 0.f;
  }
}

const atools::grib::WindPosList *WindReporter::getWindForRect(const Marble::GeoDataLatLonBox& rect,
                                                              const MapLayer *mapLayer, bool lazy, bool& overflow)
{
  atools::grib::WindQuery *windQuery = isWindManual() ? windQueryManual : windQueryOnline;
  if(windQuery->hasWindData())
  {
    // Update
    windPosCache.updateCache(rect, mapLayer, queryRectInflationFactor, queryRectInflationIncrement, lazy,
                             [](const MapLayer *curLayer, const MapLayer *newLayer) -> bool
    {
      return curLayer->hasSameQueryParametersWind(newLayer);
    });

    if((windPosCache.list.isEmpty() && !lazy) || cachedLevel != currentLevel) // Force update if level has changed
    {
      windPosCache.clear();
      for(const Marble::GeoDataLatLonBox& box :
          query::splitAtAntiMeridian(rect, queryRectInflationFactor, queryRectInflationIncrement))
      {
        atools::geo::Rect r(box.west(Marble::GeoDataCoordinates::Degree),
                            box.north(Marble::GeoDataCoordinates::Degree),
                            box.east(Marble::GeoDataCoordinates::Degree),
                            box.south(Marble::GeoDataCoordinates::Degree));

        atools::grib::WindPosVector windPosVector;
        windQuery->getWindForRect(windPosVector, r, getAltitude());
        windPosCache.list.append(windPosVector.toList());
        cachedLevel = currentLevel;
      }
    }
    overflow = windPosCache.validate(queryMaxRows);
    return &windPosCache.list;
  }
  return nullptr;
}

atools::grib::WindPos WindReporter::getWindForPos(const atools::geo::Pos& pos, float altFeet)
{
  atools::grib::WindQuery *windQuery = isWindManual() ? windQueryManual : windQueryOnline;
  atools::grib::WindPos wp;
  if(windQuery->hasWindData())
  {
    wp.pos = pos;
    wp.wind = windQuery->getWindForPos(pos.alt(altFeet));
  }
  return wp;
}

atools::grib::WindPos WindReporter::getWindForPos(const atools::geo::Pos& pos)
{
  return getWindForPos(pos, pos.getAltitude());
}

atools::grib::Wind WindReporter::getWindForPosRoute(const atools::geo::Pos& pos)
{
  return (isWindManual() ? windQueryManual : windQueryOnline)->getWindForPos(pos);
}

atools::grib::Wind WindReporter::getWindForLineRoute(const atools::geo::Pos& pos1, const atools::geo::Pos& pos2)
{
  return (isWindManual() ? windQueryManual : windQueryOnline)->getWindAverageForLine(pos1, pos2);
}

atools::grib::Wind WindReporter::getWindForLineRoute(const atools::geo::Line& line)
{
  return getWindForLineRoute(line.getPos1(), line.getPos2());
}

atools::grib::Wind WindReporter::getWindForLineStringRoute(const atools::geo::LineString& line)
{
  return (isWindManual() ? windQueryManual : windQueryOnline)->getWindAverageForLineString(line);
}

atools::grib::WindPosVector WindReporter::getWindStackForPos(const atools::geo::Pos& pos, QVector<int> altitudesFt)
{
  atools::grib::WindPosVector winds;
  atools::grib::WindQuery *windQuery = isWindManual() ? windQueryManual : windQueryOnline;

  if(windQuery->hasWindData())
  {
    float curAlt = getAltitude();
    atools::grib::WindPos wp;

    // Collect wind for all levels
    for(int i = 0; i < altitudesFt.size(); i++)
    {
      float alt = altitudesFt.at(i) == wind::AGL ? 260.f : altitudesFt.at(i);
      float altNext = i < altitudesFt.size() - 1 ? altitudesFt.at(i + 1) : 100000.f;

      // Get wind for layer/altitude
      wp.pos = pos.alt(alt);
      if(currentSource != wind::NOAA && altitudesFt.at(i) == wind::AGL)
        wp.wind = {map::INVALID_COURSE_VALUE, map::INVALID_SPEED_VALUE};
      else
        wp.wind = windQuery->getWindForPos(wp.pos);
      winds.append(wp);

      if(currentLevel == wind::FLIGHTPLAN && curAlt > alt && curAlt < altNext)
      {
        // Insert flight plan altitude if selected in GUI
        wp.pos = pos.alt(curAlt);
        wp.wind = windQuery->getWindForPos(wp.pos);
        winds.append(wp);
      }
    }
  }
  return winds;
}

atools::grib::WindPosVector WindReporter::getWindStackForPos(const atools::geo::Pos& pos)
{
  return getWindStackForPos(pos, levelsTooltip);
}

void WindReporter::updateManualRouteWinds()
{
  windQueryManual->initFromFixedModel(NavApp::getAircraftPerfController()->getWindDir(),
                                      NavApp::getAircraftPerfController()->getWindSpeed(),
                                      NavApp::getRoute().getCruisingAltitudeFeet());
}

#ifdef DEBUG_INFORMATION
QString WindReporter::getDebug(const atools::geo::Pos& pos)
{
  atools::grib::WindQuery *windQuery = isWindManual() ? windQueryManual : windQueryOnline;
  return windQuery->getDebug(pos);
}

#endif
