/*
 * Copyright (c) 2015-2016 Joshua Snyder
 * Distributed under the GNU GPL v3. For full terms see the file LICENSE
 *
 * This file is part of Shticker Book Rewritten.
 *
 * Shticker Book Rewritten is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Shticker Book Rewritten is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Shticker Book Rewritten.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PATCHWORKER_H
#define PATCHWORKER_H

#include <QObject>


class PatchWorker : public QObject
{
    Q_OBJECT
public:
    explicit PatchWorker(QObject *parent = 0);
    int patchFile(QString, QString);

signals:

public slots:

private:
    off_t offtin(u_char *);
    int bsdiff_patch(char *, char *, char *);
};

#endif // PATCHWORKER_H
