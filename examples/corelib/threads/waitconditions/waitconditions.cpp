// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2022 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com, author Giuseppe D'Angelo <giuseppe.dangelo@kdab.com>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR BSD-3-Clause

#include <QAtomicInt>
#include <QCoreApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QRandomGenerator>
#include <QThread>
#include <QWaitCondition>

#include <stdio.h>
#include <stdlib.h>

//! [0]
constexpr int DataSize = 100000;
constexpr int BufferSize = 8192;

QMutex mutex; // protects the buffer
char buffer[BufferSize];

QWaitCondition bufferNotEmpty;
QWaitCondition bufferNotFull;

QAtomicInt numUsedBytes;
//! [0]

//! [1]
class Producer : public QThread
//! [1] //! [2]
{
public:
    explicit Producer(QObject *parent = nullptr)
        : QThread(parent)
    {
    }

private:
    void run() override
    {
        for (int i = 0; i < DataSize; ++i) {
            {
                const QMutexLocker locker(&mutex);
                while (numUsedBytes.loadAcquire() == BufferSize)
                    bufferNotFull.wait(&mutex);
            }

            buffer[i % BufferSize] = "ACGT"[QRandomGenerator::global()->bounded(4)];

            numUsedBytes.fetchAndAddRelease(1);
            bufferNotEmpty.wakeAll();
        }
    }
};
//! [2]

//! [3]
class Consumer : public QThread
//! [3] //! [4]
{
public:
    explicit Consumer(QObject *parent = nullptr)
        : QThread(parent)
    {
    }

private:
    void run() override
    {
        for (int i = 0; i < DataSize; ++i) {
            {
                const QMutexLocker locker(&mutex);
                while (numUsedBytes.loadAcquire() == 0)
                    bufferNotEmpty.wait(&mutex);
            }

            fprintf(stderr, "%c", buffer[i % BufferSize]);

            numUsedBytes.fetchAndAddRelease(-1);
            bufferNotFull.wakeAll();
        }
        fprintf(stderr, "\n");
    }
};
//! [4]


//! [5]
int main(int argc, char *argv[])
//! [5] //! [6]
{
    QCoreApplication app(argc, argv);
    Producer producer;
    Consumer consumer;
    producer.start();
    consumer.start();
    producer.wait();
    consumer.wait();
    return 0;
}
//! [6]

