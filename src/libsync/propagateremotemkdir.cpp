/*
 * Copyright (C) by Olivier Goffart <ogoffart@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "propagateremotemkdir.h"
#include "owncloudpropagator_p.h"
#include "account.h"
#include "common/syncjournalfilerecord.h"
#include "propagateuploadencrypted.h"
#include "propagateremotedelete.h"
#include "common/asserts.h"
#include "encryptfolderjob.h"

#include <QFile>
#include <QLoggingCategory>

namespace OCC {

Q_LOGGING_CATEGORY(lcPropagateRemoteMkdir, "nextcloud.sync.propagator.remotemkdir", QtInfoMsg)

void PropagateRemoteMkdir::start()
{
    if (propagator()->_abortRequested.fetchAndAddRelaxed(0))
        return;

    qCDebug(lcPropagateRemoteMkdir) << _item->_file;

    propagator()->_activeJobList.append(this);

    if (!_deleteExisting) {
        slotMkdir();
        return;
    }

    _job = new DeleteJob(propagator()->account(),
        propagator()->_remoteFolder + _item->_file,
        this);
    connect(static_cast<DeleteJob*>(_job.data()), &DeleteJob::finishedSignal,
            this, &PropagateRemoteMkdir::slotMkdir);
    _job->start();
}

void PropagateRemoteMkdir::slotStartMkcolJob()
{
    if (propagator()->_abortRequested.fetchAndAddRelaxed(0))
        return;

    qCDebug(lcPropagateRemoteMkdir) << _item->_file;

    _job = new MkColJob(propagator()->account(),
        propagator()->_remoteFolder + _item->_file,
        this);
    connect(_job, SIGNAL(finished(QNetworkReply::NetworkError)), this, SLOT(slotMkcolJobFinished()));
    _job->start();
}

void PropagateRemoteMkdir::slotStartEncryptedMkcolJob(const QString &path, const QString &filename, quint64 size)
{
    Q_UNUSED(path)
    Q_UNUSED(size)

    if (propagator()->_abortRequested.fetchAndAddRelaxed(0))
        return;

    qDebug() << filename;
    qCDebug(lcPropagateRemoteMkdir) << filename;

    auto job = new MkColJob(propagator()->account(),
                            propagator()->_remoteFolder + filename,
                            this);
    connect(job, qOverload<QNetworkReply::NetworkError>(&MkColJob::finished),
            _uploadEncryptedHelper, &PropagateUploadEncrypted::unlockFolder);
    connect(job, qOverload<QNetworkReply::NetworkError>(&MkColJob::finished),
            this, &PropagateRemoteMkdir::slotMkcolJobFinished);
    _job = job;
    _job->start();
}

void PropagateRemoteMkdir::abort(PropagatorJob::AbortType abortType)
{
    if (_job && _job->reply())
        _job->reply()->abort();

    if (abortType == AbortType::Asynchronous) {
        emit abortFinished();
    }
}

void PropagateRemoteMkdir::setDeleteExisting(bool enabled)
{
    _deleteExisting = enabled;
}

void PropagateRemoteMkdir::slotMkdir()
{
    const auto rootPath = [=]() {
        const auto result = propagator()->_remoteFolder;
        if (result.startsWith('/')) {
            return result.mid(1);
        } else {
            return result;
        }
    }();
    const auto path = _item->_file;
    const auto slashPosition = path.lastIndexOf('/');
    const auto parentPath = slashPosition >= 0 ? path.left(slashPosition) : QString();

    SyncJournalFileRecord parentRec;
    bool ok = propagator()->_journal->getFileRecord(parentPath, &parentRec);
    if (!ok) {
        done(SyncFileItem::NormalError);
        return;
    }

    const auto remoteParentPath = parentRec._e2eMangledName.isEmpty() ? parentPath : parentRec._e2eMangledName;
    const auto absoluteRemoteParentPath = remoteParentPath.isEmpty() ? rootPath : rootPath + remoteParentPath + '/';
    const auto account = propagator()->account();

    if (!account->capabilities().clientSideEncryptionAvailable() ||
        (!account->e2e()->isFolderEncrypted(absoluteRemoteParentPath) &&
         !account->e2e()->isAnyParentFolderEncrypted(absoluteRemoteParentPath))) {
        slotStartMkcolJob();
        return;
    }

    // We should be encrypted as well since our parent is
    _uploadEncryptedHelper = new PropagateUploadEncrypted(propagator(), remoteParentPath, _item, this);
    connect(_uploadEncryptedHelper, &PropagateUploadEncrypted::folderNotEncrypted,
      this, &PropagateRemoteMkdir::slotStartMkcolJob);
    connect(_uploadEncryptedHelper, &PropagateUploadEncrypted::finalized,
      this, &PropagateRemoteMkdir::slotStartEncryptedMkcolJob);
    connect(_uploadEncryptedHelper, &PropagateUploadEncrypted::error,
      []{ qCDebug(lcPropagateRemoteMkdir) << "Error setting up encryption."; });
    _uploadEncryptedHelper->start();
}

void PropagateRemoteMkdir::slotMkcolJobFinished()
{
    propagator()->_activeJobList.removeOne(this);

    ASSERT(_job);

    QNetworkReply::NetworkError err = _job->reply()->error();
    _item->_httpErrorCode = _job->reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (_item->_httpErrorCode == 405) {
        // This happens when the directory already exists. Nothing to do.
    } else if (err != QNetworkReply::NoError) {
        SyncFileItem::Status status = classifyError(err, _item->_httpErrorCode,
            &propagator()->_anotherSyncNeeded);
        done(status, _job->errorString());
        return;
    } else if (_item->_httpErrorCode != 201) {
        // Normally we expect "201 Created"
        // If it is not the case, it might be because of a proxy or gateway intercepting the request, so we must
        // throw an error.
        done(SyncFileItem::NormalError,
            tr("Wrong HTTP code returned by server. Expected 201, but received \"%1 %2\".")
                .arg(_item->_httpErrorCode)
                .arg(_job->reply()->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString()));
        return;
    }

    _item->_responseTimeStamp = _job->responseTimestamp();
    _item->_fileId = _job->reply()->rawHeader("OC-FileId");

    if (_item->_fileId.isEmpty()) {
        // Owncloud 7.0.0 and before did not have a header with the file id.
        // (https://github.com/owncloud/core/issues/9000)
        // So we must get the file id using a PROPFIND
        // This is required so that we can detect moves even if the folder is renamed on the server
        // while files are still uploading
        propagator()->_activeJobList.append(this);
        auto propfindJob = new PropfindJob(_job->account(), _job->path(), this);
        propfindJob->setProperties(QList<QByteArray>() << "getetag"
                                                       << "http://owncloud.org/ns:id");
        QObject::connect(propfindJob, &PropfindJob::result, this, &PropagateRemoteMkdir::propfindResult);
        QObject::connect(propfindJob, &PropfindJob::finishedWithError, this, &PropagateRemoteMkdir::propfindError);
        propfindJob->start();
        _job = propfindJob;
        return;
    }

    if (!_uploadEncryptedHelper) {
        success();
    } else {
        // We still need to mark that folder encrypted
        propagator()->_activeJobList.append(this);
        auto job = new OCC::EncryptFolderJob(propagator()->account(), _job->path(), _item->_fileId, this);
        connect(job, &OCC::EncryptFolderJob::finished, this, &PropagateRemoteMkdir::slotEncryptFolderFinished);
        job->start();
    }
}

void PropagateRemoteMkdir::slotEncryptFolderFinished()
{
    qCDebug(lcPropagateRemoteMkdir) << "Success making the new folder encrypted";
    propagator()->_activeJobList.removeOne(this);
    success();
}

void PropagateRemoteMkdir::propfindResult(const QVariantMap &result)
{
    propagator()->_activeJobList.removeOne(this);
    if (result.contains("getetag")) {
        _item->_etag = result["getetag"].toByteArray();
    }
    if (result.contains("id")) {
        _item->_fileId = result["id"].toByteArray();
    }
    success();
}

void PropagateRemoteMkdir::propfindError()
{
    // ignore the PROPFIND error
    propagator()->_activeJobList.removeOne(this);
    done(SyncFileItem::Success);
}

void PropagateRemoteMkdir::success()
{
    // save the file id already so we can detect rename or remove
    SyncJournalFileRecord record = _item->toSyncJournalFileRecordWithInode(propagator()->_localDir + _item->destination());
    if (!propagator()->_journal->setFileRecord(record)) {
        done(SyncFileItem::FatalError, tr("Error writing metadata to the database"));
        return;
    }

    done(SyncFileItem::Success);
}
}
