/*
 * Vizkit3dWorld.cpp
 *
 *  Created on: Apr 20, 2015
 *      Author: gustavoneves
 */

#include <iostream>
#include <QString>
#include <QtGui>
#include <QtCore>
#include <QEvent>

#include <boost/algorithm/string.hpp>
#include <vizkit3d_world/Vizkit3dWorld.hpp>
#include <osgViewer/View>
#include <osgGA/CameraManipulator>
#include <osgGA/FirstPersonManipulator>

#include "Utils.hpp"

namespace vizkit3d_world {

Vizkit3dWorld::Vizkit3dWorld(std::string path, std::vector<std::string> modelPaths, bool showGui)
    : worldPath(path),
      modelPaths(modelPaths),
      showGui(showGui),
      widget(NULL),
      running(false),
      currentFrame(new base::samples::frame::Frame())
{
    loadGazeboModelPaths(modelPaths);
}

Vizkit3dWorld::~Vizkit3dWorld()
{
    deinitialize();
}

void Vizkit3dWorld::initialize() {

    if (running) {
        return;
    }

    // start the event loop thread
    boost::mutex::scoped_lock lock(mut);
    guiThread = boost::thread( boost::bind( &Vizkit3dWorld::run, this ));

    //set thread priority to max
    setthread_priority_max(guiThread);

    //wait until the settings is finished in the event loop thread
    cond.wait(lock);
    usleep(100);
}

void Vizkit3dWorld::deinitialize() {

    if (running) {

        { //finish Qt event loop thread
            boost::mutex::scoped_lock lock(mut);

            app->closeAllWindows();

            appQuit = true;

            processEventCondition.notify_all();

            cond.wait(lock);
        }

        //close all openned windows

        //the correct way is to use guiThread.join()
        //but if showGui is "false" then the QApplication::exec is frozen
        //to solve this problem join the thread for 1000
        guiThread.try_join_for(boost::chrono::milliseconds(1000));
    }
}

void Vizkit3dWorld::wait(){
    /**
     * wait until Qt event loop thread stop
     */
    boost::mutex::scoped_lock lock(mut);
    cond.wait(lock);
}

/**
 * Thread procedure with Qt event loop thread
 */
void Vizkit3dWorld::run() {

    QEventLoop::ProcessEventsFlags flags = QEventLoop::ExcludeSocketNotifiers;

    { //start QApplication, vizkit3d and load SDF models
        boost::mutex::scoped_lock lock(mut);
        int argc = 1;
        char *argv[] = { "vizkit3d_world" };

        app = new QApplication(argc, argv);

        //intercept the custom events
        customEventReceiver = new events::CustomEventReceiver(this);

        //main widget to store the plugins and performs the GUI events
        widget = new vizkit3d::Vizkit3DWidget();

        //remove the close button from window title
        widget->setWindowFlags(widget->windowFlags() & ~Qt::WindowCloseButtonHint); //remove close button from windows title
        widget->setWindowFlags(widget->windowFlags() & ~Qt::WindowMaximizeButtonHint); //remove maximize button from windows title
        widget->setFixedSize(800, 600); //set the window size
        widget->getPropertyWidget()->hide(); //hide the right property widget

        while (app->startingUp()) usleep(100);

        //load the world sdf file and created the vizkit3d::RobotVisualization models
        //It is necessary to create the vizkit3d plugins in the same thread of QApplication
        loadFromFile(worldPath);
        attachPlugins();

        //apply the tranformations in each model
        applyTransformations();

        if (showGui) {
            widget->show();
        }
        else {
            /**
             * if the widget is not showing, not process the user input events
             */
            flags |= QEventLoop::ExcludeUserInputEvents;
        }

        running = true;
        appQuit = false;
    }

    cond.notify_one();

    /**
     * custom Qt event loop
     *
     * blocking wait for notification
     * when notification is received process the Qt events
     */
    while (!appQuit)
    {
        boost::mutex::scoped_lock lock(processEventMutex);

        //this condition is used to notify the calling function (notifyEvents) that the events were processed
        notifyEventCondition.notify_all();
        //block the thread until receiving a notification
        processEventCondition.wait(lock);

        {
            /**
            * this mutex is used to synchronize the calling function (notifyEvents) with event loop thread
            * using this mutex, if notifyEvents is called, then notifyEvents is blocked until processEvents is finished
            */
            boost::mutex::scoped_lock lock(notifyEventMutex);
            app->processEvents(flags);
        }

    }

    {   // finalize Qt event loop
        // remove objects from memory
        boost::mutex::scoped_lock lock(mut);
        delete widget;
        widget = NULL;
        delete app;
        app = NULL;
        toSdfElement.clear();
        robotVizMap.clear();
        running = false;
    }

    cond.notify_one();
}

void Vizkit3dWorld::notifyEvents()
{

    //this mutex is used to synchronize event loop thread
    //this mutex also doesn't allow to execute notifyEvents in parallel
    boost::mutex::scoped_lock lock(notifyEventMutex);
    //notify event loop thread unblocking it
    processEventCondition.notify_all();
    //wait until process events is finalized
    notifyEventCondition.wait(lock);
    //this sleep is important because it blocks the current thread, allowing the other threads to get time on the processor
    usleep(250);
}

void Vizkit3dWorld::loadFromFile(std::string path) {
    std::ifstream file(path.c_str());
    std::string str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    loadFromString(str);
}

void Vizkit3dWorld::loadFromString(const std::string xml) {
    sdf::SDFPtr sdf(new sdf::SDF);
    if (!sdf::init(sdf)) {
        throw std::runtime_error("unable to initialize sdf");
    }

    if (!sdf::readString(xml, sdf)) {
        throw std::invalid_argument("unable to load sdf from string " + xml + "\n");
    }

    if (!sdf->root->HasElement("world")) {
        throw std::invalid_argument("the SDF doesn't have a <world> tag\n");
    }

    makeWorld(sdf->root->GetElement("world"), sdf->version);
}

void Vizkit3dWorld::loadGazeboModelPaths(std::vector<std::string> modelPaths) {

    for (std::vector<std::string>::iterator it = modelPaths.begin(); it != modelPaths.end(); it++){
        sdf::addURIPath("model://", *it);
    }

    std::string home = getEnv("HOME");

    sdf::addURIPath("model://", home + "/.gazebo/models");

    std::string path = getEnv("GAZEBO_MODEL_PATH") + std::string(":") + getEnv("PATH");

    std::vector<std::string> vec;
    boost::algorithm::split(vec, path, boost::algorithm::is_any_of(":"), boost::algorithm::token_compress_on);

    for (std::vector<std::string>::iterator it = vec.begin(); it != vec.end(); it++){
        if (!(*it).empty()){
            sdf::addURIPath("model://", *it);
        }
    }
}

void Vizkit3dWorld::makeWorld(sdf::ElementPtr sdf, std::string version) {

    if (sdf->HasElement("model")) {
        worldName = sdf->Get<std::string>("name");

        std::map<std::string, int> robotVizCountMap;

        sdf::ElementPtr modelElem = sdf->GetElement("model");

        while (modelElem) {

            std::string modelName = modelElem->Get<std::string>("name");

            /**
             * this code is used to add the models with the same name
             * but it is necessary to change control/kdl_parser
             * and control/sdf_ruby to change the base segment name
             */
            if (robotVizMap.find(modelName) == robotVizMap.end()){
                robotVizCountMap.insert(std::make_pair(modelName, 0));
            }
            else {
                std::ostringstream buf;
                buf << modelName << "_" << (robotVizCountMap[modelName]++);
                modelName = buf.str();
            }

            vizkit3d::RobotVisualization* robotViz = robotVizFromSdfModel(modelElem, modelName, version);
            robotVizMap.insert(std::make_pair(modelName, robotViz));

            modelElem = modelElem->GetNextElement("model");

        }
    }
}

vizkit3d::RobotVisualization* Vizkit3dWorld::robotVizFromSdfModel(sdf::ElementPtr sdf_model, std::string modelName, std::string version) {

    //vizkit3d plugin with model defined in the sdf model
    vizkit3d::RobotVisualization* robotViz = new vizkit3d::RobotVisualization();

    std::string prefix;
    std::string modelstr = "<sdf version='" +  version + "'>" + sdf_model->ToString(prefix) + "</sdf>";
    sdf::SDF sdf;
    sdf.SetFromString(modelstr);
    robotViz->loadFromString(QString(sdf.ToString().c_str()), QString("sdf"));
    robotViz->setPluginName(modelName.c_str());
    robotViz->relocateRoot(modelName);

    toSdfElement.insert(std::make_pair(modelName, sdf_model));


    return robotViz;
}

RobotVizMap Vizkit3dWorld::getRobotVizMap() {
    return robotVizMap;
}

void Vizkit3dWorld::attachPlugins()
{
    for (RobotVizMap::iterator it = robotVizMap.begin(); it != robotVizMap.end(); it++){
        widget->addPlugin(it->second);
        it->second->setParent(widget);
        //it is necessary to add to widget first and set the parent widget
        it->second->setVisualizationFrame(it->first.c_str());
    }
}

vizkit3d::RobotVisualization* Vizkit3dWorld::getRobotViz(std::string name)
{
    RobotVizMap::iterator robotviz_it = robotVizMap.find(name);
    return (robotviz_it == robotVizMap.end()) ? NULL : robotviz_it->second;
}

void Vizkit3dWorld::setJoints(std::string modelName, base::samples::Joints joints) {
    vizkit3d::RobotVisualization *viz;
    if ((viz = getRobotViz(modelName)) != NULL) {
        viz->updateData(joints);
    }
}

sdf::ElementPtr Vizkit3dWorld::getSdfElement(std::string name) {
    std::map<std::string, sdf::ElementPtr>::iterator it = toSdfElement.find(name);
    return (it == toSdfElement.end()) ? sdf::ElementPtr() : it->second;
}

void Vizkit3dWorld::applyTransformations() {

    for (RobotVizMap::iterator it = robotVizMap.begin();
            it != robotVizMap.end(); it++){

        sdf::ElementPtr sdfModel = getSdfElement(it->first);

        sdf::Pose pose =  sdfModel->GetElement("pose")->Get<sdf::Pose>();

        applyTransformation(worldName, it->first,
                            QVector3D(pose.pos.x, pose.pos.y, pose.pos.z),
                            QQuaternion(pose.rot.w, pose.rot.x, pose.rot.y, pose.rot.z));

    }
}

void Vizkit3dWorld::applyTransformation(base::samples::RigidBodyState rbs) {
    applyTransformation(rbs.targetFrame,
                        rbs.sourceFrame,
                        rbs.position,
                        rbs.orientation);
}

void Vizkit3dWorld::applyTransformation(std::string targetFrame, std::string sourceFrame, base::Position position, base::Orientation orientation) {
    applyTransformation(targetFrame ,sourceFrame,
                        QVector3D(position.x(), position.y(), position.z()),
                        QQuaternion(orientation.w(), orientation.x(), orientation.y(), orientation.z()));
}

void Vizkit3dWorld::applyTransformation(std::string targetFrame, std::string sourceFrame, QVector3D position, QQuaternion orientation) {

    if (widget) {
        widget->setTransformation(QString::fromStdString(targetFrame),
                                  QString::fromStdString(sourceFrame),
                                  position,
                                  orientation);
    }
}

void Vizkit3dWorld::customEvent(QEvent *e) {
    if (e->type() == events::TransformationEventId) {
        events::TransformationEvent *te = (events::TransformationEvent*)e;
        applyTransformation(te->pose);
    }
    else if (e->type() == events::GrabbingEventId) {
        events::GrabbingEvent *ge = (events::GrabbingEvent*)e;
        enableGrabbing(ge->enableGrabbing);
    }
    else if (e->type() == events::GrabEventId) {
        grabbedImage = widget->grab();
    }
}

void Vizkit3dWorld::setTransformation(base::samples::RigidBodyState rbs) {
    //the function setTransformation must be called from Qt event loop thread
    //if setTransformation was called in another thread the application breaks
    QEvent *evt = new events::TransformationEvent(rbs);
    app->postEvent(customEventReceiver, evt);
}

void Vizkit3dWorld::setCameraPos(base::samples::RigidBodyState pose) {
    osgViewer::View *view = widget->getView(0);

    osg::Vec3d pos; //camera position
    osg::Quat rot; //camera rotation

    //convert RigidBodyState to osg::Matrixd
    osg::Matrixd m;
    m.setTrans(osg::Vec3f(pose.position.x(), pose.position.y(), pose.position.z()));
    m.setRotate(osg::Quat(pose.orientation.x(),
                          pose.orientation.y(),
                          pose.orientation.z(),
                          pose.orientation.w()));

    osgGA::StandardManipulator *manipulator = dynamic_cast<osgGA::StandardManipulator*>(view->getCameraManipulator());
    manipulator->setByMatrix(m);
}


//post event to enable grabbing
void Vizkit3dWorld::postEnableGrabbing() {
    QEvent *evt = new events::GrabbingEvent(true);
    app->postEvent(customEventReceiver, evt);
}


//post event to disable grabbing
void Vizkit3dWorld::postDisableGrabbing()
{
    QEvent *evt = new events::GrabbingEvent(false);
    app->postEvent(customEventReceiver, evt);
}

//internal enable and disable grabbing
void Vizkit3dWorld::enableGrabbing(bool value)
{

    if (value) {
        widget->enableGrabbing();
        grabbedImage = widget->grab();
    }
    else {
        widget->disableGrabbing();
    }
}

//post event to grab image
void Vizkit3dWorld::postGrabImage()
{
    QEvent *evt = new QEvent(QEvent::Type(events::GrabEventId));
    app->postEvent(customEventReceiver, evt);
}

//grab image
//post event to grab image and notify event loop
QImage Vizkit3dWorld::grabImage()
{
    postGrabImage();
    notifyEvents();
    return grabbedImage;
}

//grab frame
//convert QImage to base::samples::frame::Frame
base::samples::frame::Frame* Vizkit3dWorld::grabFrame()
{
    QImage image = grabImage();

    cvtQImageToFrame(image, *currentFrame, (widget->isVisible() && !widget->isMinimized()));
    currentFrame->time = base::Time::now();
    return currentFrame;
}

}
