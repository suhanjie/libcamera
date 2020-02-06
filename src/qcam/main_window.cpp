/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * main_window.cpp - qcam - Main application window
 */

#include <iomanip>
#include <iostream>
#include <string>
#include <sys/mman.h>

#include <QCoreApplication>
#include <QIcon>
#include <QInputDialog>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <libcamera/camera_manager.h>
#include <libcamera/version.h>

#include "main_window.h"
#include "viewfinder.h"

using namespace libcamera;

MainWindow::MainWindow(CameraManager *cm, const OptionsParser::Options &options)
	: options_(options), allocator_(nullptr), isCapturing_(false)
{
	int ret;

	createToolbars(cm);

	title_ = "QCam " + QString::fromStdString(CameraManager::version());
	setWindowTitle(title_);
	connect(&titleTimer_, SIGNAL(timeout()), this, SLOT(updateTitle()));

	viewfinder_ = new ViewFinder(this);
	setCentralWidget(viewfinder_);
	adjustSize();

	ret = openCamera(cm);
	if (!ret) {
		ret = startCapture();
	}

	if (ret < 0)
		quit();
}

MainWindow::~MainWindow()
{
	if (camera_) {
		stopCapture();
		camera_->release();
		camera_.reset();
	}
}

int MainWindow::createToolbars(CameraManager *cm)
{
	QAction *action;

	toolbar_ = addToolBar("");

	action = toolbar_->addAction(QIcon(":x-circle.svg"), "Quit");
	connect(action, &QAction::triggered, this, &MainWindow::quit);

	QAction *cameraAction = new QAction("&Cameras", this);
	toolbar_->addAction(cameraAction);

	QToolButton *cameraButton = dynamic_cast<QToolButton *>(toolbar_->widgetForAction(cameraAction));

	cameraButton->setPopupMode(QToolButton::InstantPopup);

	for (const std::shared_ptr<Camera> &cam : cm->cameras()) {
		action = new QAction(QString::fromStdString(cam->name()));
		cameraButton->addAction(action);
		connect(action, &QAction::triggered, this, [=]() { this->setCamera(cam); });
	}

	action = toolbar_->addAction(QIcon(":play-circle.svg"), "start");
	connect(action, &QAction::triggered, this, &MainWindow::startCapture);

	toolbar_->addAction(QIcon(":pause-circle.svg"), "pause");
	/* TODO: Connect an action to perform when 'pause' requested? or remove */

	action = toolbar_->addAction(QIcon(":stop-circle.svg"), "stop");
	connect(action, &QAction::triggered, this, &MainWindow::stopCapture);

	return 0;
}

void MainWindow::quit()
{
	QTimer::singleShot(0, QCoreApplication::instance(),
			   &QCoreApplication::quit);
}

void MainWindow::updateTitle()
{
	unsigned int duration = frameRateInterval_.elapsed();
	unsigned int frames = framesCaptured_ - previousFrames_;
	double fps = frames * 1000.0 / duration;

	/* Restart counters. */
	frameRateInterval_.start();
	previousFrames_ = framesCaptured_;

	setWindowTitle(title_ + " : " + QString::number(fps, 'f', 2) + " fps");
}

int MainWindow::setCamera(const std::shared_ptr<Camera> &cam)
{
	std::cout << "Chose " << cam->name() << std::endl;

	if (cam->acquire()) {
		std::cout << "Failed to acquire camera" << std::endl;
		return -EBUSY;
	}

	std::cout << "Switching to camera " << cam->name() << std::endl;

	stopCapture();
	camera_->release();

	/*
	 * If we don't disconnect this signal, it will persist (and get
	 * re-added and thus duplicated later if we ever switch back to an
	 * previously streamed camera). This causes all sorts of pain.
	 *
	 * Perhaps releasing a camera should disconnect all (public?) connected
	 * signals forcefully!
	 */
	camera_->requestCompleted.disconnect(this, &MainWindow::requestComplete);
	camera_ = cam;
	camera_->requestCompleted.connect(this, &MainWindow::requestComplete);

	startCapture();

	return 0;
}

std::string MainWindow::chooseCamera(CameraManager *cm)
{
	QStringList cameras;
	bool result;

	if (cm->cameras().size() == 1)
		return cm->cameras()[0]->name();

	for (const std::shared_ptr<Camera> &cam : cm->cameras())
		cameras.append(QString::fromStdString(cam->name()));

	QString name = QInputDialog::getItem(this, "Select Camera",
					     "Camera:", cameras, 0,
					     false, &result);
	if (!result)
		return std::string();

	return name.toStdString();
}

int MainWindow::openCamera(CameraManager *cm)
{
	std::string cameraName;

	if (options_.isSet(OptCamera))
		cameraName = static_cast<std::string>(options_[OptCamera]);
	else
		cameraName = chooseCamera(cm);

	if (cameraName == "")
		return -EINVAL;

	camera_ = cm->get(cameraName);
	if (!camera_) {
		std::cout << "Camera " << cameraName << " not found"
			  << std::endl;
		return -ENODEV;
	}

	if (camera_->acquire()) {
		std::cout << "Failed to acquire camera" << std::endl;
		camera_.reset();
		return -EBUSY;
	}

	std::cout << "Using camera " << camera_->name() << std::endl;

	camera_->requestCompleted.connect(this, &MainWindow::requestComplete);

	return 0;
}

int MainWindow::startCapture()
{
	int ret;

	config_ = camera_->generateConfiguration({ StreamRole::VideoRecording });

	StreamConfiguration &cfg = config_->at(0);
	if (options_.isSet(OptSize)) {
		const std::vector<OptionValue> &sizeOptions =
			options_[OptSize].toArray();

		/* Set desired stream size if requested. */
		for (const auto &value : sizeOptions) {
			KeyValueParser::Options opt = value.toKeyValues();

			if (opt.isSet("width"))
				cfg.size.width = opt["width"];

			if (opt.isSet("height"))
				cfg.size.height = opt["height"];
		}
	}

	CameraConfiguration::Status validation = config_->validate();
	if (validation == CameraConfiguration::Invalid) {
		std::cerr << "Failed to create valid camera configuration";
		return -EINVAL;
	}

	if (validation == CameraConfiguration::Adjusted) {
		std::cout << "Stream size adjusted to "
			  << cfg.size.toString() << std::endl;
	}

	ret = camera_->configure(config_.get());
	if (ret < 0) {
		std::cout << "Failed to configure camera" << std::endl;
		return ret;
	}

	Stream *stream = cfg.stream();
	ret = viewfinder_->setFormat(cfg.pixelFormat, cfg.size.width,
				     cfg.size.height);
	if (ret < 0) {
		std::cout << "Failed to set viewfinder format" << std::endl;
		return ret;
	}

	adjustSize();

	allocator_ = FrameBufferAllocator::create(camera_);
	ret = allocator_->allocate(stream);
	if (ret < 0) {
		std::cerr << "Failed to allocate capture buffers" << std::endl;
		return ret;
	}

	std::vector<Request *> requests;
	for (const std::unique_ptr<FrameBuffer> &buffer : allocator_->buffers(stream)) {
		Request *request = camera_->createRequest();
		if (!request) {
			std::cerr << "Can't create request" << std::endl;
			ret = -ENOMEM;
			goto error;
		}

		ret = request->addBuffer(stream, buffer.get());
		if (ret < 0) {
			std::cerr << "Can't set buffer for request" << std::endl;
			goto error;
		}

		requests.push_back(request);

		/* Map memory buffers and cache the mappings. */
		const FrameBuffer::Plane &plane = buffer->planes().front();
		void *memory = mmap(NULL, plane.length, PROT_READ, MAP_SHARED,
				    plane.fd.fd(), 0);
		mappedBuffers_[plane.fd.fd()] =
			std::make_pair(memory, plane.length);
	}

	titleTimer_.start(2000);
	frameRateInterval_.start();
	previousFrames_ = 0;
	framesCaptured_ = 0;
	lastBufferTime_ = 0;

	ret = camera_->start();
	if (ret) {
		std::cout << "Failed to start capture" << std::endl;
		goto error;
	}

	for (Request *request : requests) {
		ret = camera_->queueRequest(request);
		if (ret < 0) {
			std::cerr << "Can't queue request" << std::endl;
			goto error;
		}
	}

	isCapturing_ = true;
	return 0;

error:
	for (Request *request : requests)
		delete request;

	for (auto &iter : mappedBuffers_) {
		void *memory = iter.second.first;
		unsigned int length = iter.second.second;
		munmap(memory, length);
	}
	mappedBuffers_.clear();

	return ret;
}

void MainWindow::stopCapture()
{
	if (!isCapturing_)
		return;

	int ret = camera_->stop();
	if (ret)
		std::cout << "Failed to stop capture" << std::endl;

	for (auto &iter : mappedBuffers_) {
		void *memory = iter.second.first;
		unsigned int length = iter.second.second;
		munmap(memory, length);
	}
	mappedBuffers_.clear();

	delete allocator_;

	isCapturing_ = false;

	config_.reset();

	titleTimer_.stop();
	setWindowTitle(title_);
}

void MainWindow::requestComplete(Request *request)
{
	if (request->status() == Request::RequestCancelled)
		return;

	const std::map<Stream *, FrameBuffer *> &buffers = request->buffers();

	framesCaptured_++;

	FrameBuffer *buffer = buffers.begin()->second;
	const FrameMetadata &metadata = buffer->metadata();

	double fps = metadata.timestamp - lastBufferTime_;
	fps = lastBufferTime_ && fps ? 1000000000.0 / fps : 0.0;
	lastBufferTime_ = metadata.timestamp;

	std::cout << "seq: " << std::setw(6) << std::setfill('0') << metadata.sequence
		  << " bytesused: " << metadata.planes[0].bytesused
		  << " timestamp: " << metadata.timestamp
		  << " fps: " << std::fixed << std::setprecision(2) << fps
		  << std::endl;

	display(buffer);

	request = camera_->createRequest();
	if (!request) {
		std::cerr << "Can't create request" << std::endl;
		return;
	}

	for (auto it = buffers.begin(); it != buffers.end(); ++it) {
		Stream *stream = it->first;
		FrameBuffer *buffer = it->second;

		request->addBuffer(stream, buffer);
	}

	camera_->queueRequest(request);
}

int MainWindow::display(FrameBuffer *buffer)
{
	if (buffer->planes().size() != 1)
		return -EINVAL;

	const FrameBuffer::Plane &plane = buffer->planes().front();
	void *memory = mappedBuffers_[plane.fd.fd()].first;
	unsigned char *raw = static_cast<unsigned char *>(memory);
	viewfinder_->display(raw, buffer->metadata().planes[0].bytesused);

	return 0;
}
