/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * viewfinder.h - qcam - Viewfinder
 */
#ifndef __QCAM_VIEWFINDER_H__
#define __QCAM_VIEWFINDER_H__

#include <QWidget>

#include "format_converter.h"

class QImage;

class ViewFinder : public QWidget
{
public:
	ViewFinder(QWidget *parent);
	~ViewFinder();

	int setFormat(unsigned int format, unsigned int width,
		      unsigned int height);
	void display(const unsigned char *rgb, size_t size);

	QImage getCurrentImage();

protected:
	void paintEvent(QPaintEvent *) override;
	QSize sizeHint() const override;

private:
	unsigned int format_;
	unsigned int width_;
	unsigned int height_;

	FormatConverter converter_;
	QImage *image_;
};

#endif /* __QCAM_VIEWFINDER__ */
