/***************************************************************************
**
** Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (people-users@projects.maemo.org)
**
** This file is part of contactsd.
**
** If you have questions regarding the use of this file, please contact
** Nokia at people-users@projects.maemo.org.
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation
** and appearing in the file LICENSE.LGPL included in the packaging
** of this file.
**
****************************************************************************/

#include "cdtpstorage.h"

#include <TelepathyQt4/ContactCapabilities>

CDTpStorage::CDTpStorage(QObject *parent)
    : QObject(parent)
{
}

CDTpStorage::~CDTpStorage()
{
}

void CDTpStorage::syncAccount(CDTpAccount *accountWrapper)
{
    syncAccount(accountWrapper, CDTpAccount::All);
}

// TODO: Improve syncAccount so that it only updates the data that really
//       changed
void CDTpStorage::syncAccount(CDTpAccount *accountWrapper,
        CDTpAccount::Changes changes)
{
    Tp::AccountPtr account = accountWrapper->account();
    QString accountObjectPath = account->objectPath();
    const QString strLocalUID = QString::number(0x7FFFFFFF);

    qDebug() << "Syncing account" << accountObjectPath << "to storage";

    const QUrl accountUrl(QString("telepathy:%1").arg(accountObjectPath));
    QString paramAccount = account->parameters()["account"].toString();
    const QUrl imAddressUrl(QString("telepathy:%1!%2")
            .arg(accountObjectPath).arg(paramAccount));

    RDFUpdate up;

    RDFVariable imAccount(accountUrl);
    RDFVariable imAddress(imAddressUrl);

    up.addInsertion(imAccount, rdf::type::iri(), nco::IMAccount::iri());
    up.addInsertion(imAccount, nco::imAccountType::iri(), LiteralValue(account->protocol()));

    up.addInsertion(imAddress, rdf::type::iri(), nco::IMAddress::iri());
    up.addInsertion(imAddress, nco::imID::iri(), LiteralValue(paramAccount));

    if (changes & CDTpAccount::DisplayName) {
        up.addInsertion(imAccount, nco::imDisplayName::iri(), LiteralValue(account->displayName()));
    }

    if (changes & CDTpAccount::Nickname) {
       up.addInsertion(imAddress, nco::imNickname::iri(), LiteralValue(account->displayName()));
    }

    if (changes & CDTpAccount::Presence) {
        Tp::SimplePresence presence = account->currentPresence();

        up.addInsertion(imAddress, nco::imStatusMessage::iri(),
                LiteralValue(presence.statusMessage));
        up.addInsertion(imAddress, nco::imPresence::iri(),
                LiteralValue(trackerStatusFromTpPresenceStatus(presence.status)));
        up.addInsertion(imAddress, nco::presenceLastModified::iri(),
                LiteralValue(QDateTime::currentDateTime()));
    }

    // link the IMAddress to me-contact
    up.addInsertion(nco::default_contact_me::iri(), nco::contactLocalUID::iri(),
            LiteralValue(strLocalUID));
    up.addInsertion(nco::default_contact_me::iri(), nco::hasIMAddress::iri(), imAddress);
    up.addInsertion(imAccount, nco::imAccountAddress::iri(), imAddress);

    if (changes & CDTpAccount::Avatar) {
        QString fileName;
        const Tp::Avatar &avatar = account->avatar();
        // TODO: saving to disk needs to be removed here
        const bool ok = saveAccountAvatar(avatar.avatarData, avatar.MIMEType,
                QString("%1/.contacts/avatars/").arg(QDir::homePath()), fileName);
        updateAvatar(up, imAddressUrl, QUrl::fromLocalFile(fileName), ok);
    }

    ::tracker()->executeQuery(up);
}

void CDTpStorage::syncAccountContacts(CDTpAccount *accountWrapper)
{
    // TODO: return the number of contacts that were actually added
    syncAccountContacts(accountWrapper, accountWrapper->contacts(),
            QList<CDTpContact *>());
}

void CDTpStorage::syncAccountContacts(CDTpAccount *accountWrapper,
        const QList<CDTpContact *> &contactsAdded,
        const QList<CDTpContact *> &contactsRemoved)
{
    RDFUpdate updateQuery;
    Tp::AccountPtr account = accountWrapper->account();
    QString accountObjectPath = account->objectPath();
    foreach (CDTpContact *contactWrapper, contactsAdded) {
        Tp::ContactPtr contact = contactWrapper->contact();

        const QString id = contact->id();
        const QString localId = contactLocalId(accountObjectPath, id);

        const RDFVariable imContact(contactIri(localId));
        const RDFVariable imAddress(contactImAddress(accountObjectPath, id));
        const RDFVariable imAccount(QUrl(QString("telepathy:%1").arg(accountObjectPath)));
        const QDateTime datetime = QDateTime::currentDateTime();

        updateQuery.addDeletion(imContact, nie::contentLastModified::iri());

        updateQuery.addInsertion(RDFStatementList() <<
                RDFStatement(imAddress, rdf::type::iri(), nco::IMAddress::iri()) <<
                RDFStatement(imAddress, nco::imID::iri(), LiteralValue(id)));

        updateQuery.addInsertion(RDFStatementList() <<
                RDFStatement(imContact, rdf::type::iri(), nco::PersonContact::iri()) <<
                RDFStatement(imContact, nco::hasIMAddress::iri(), imAddress) <<
                RDFStatement(imContact, nco::contactLocalUID::iri(), LiteralValue(localId)) <<
                RDFStatement(imContact, nco::contactUID::iri(), LiteralValue(localId)));

        updateQuery.addInsertion(RDFStatementList() <<
                RDFStatement(imAccount, rdf::type::iri(), nco::IMAccount::iri()) <<
                RDFStatement(imAccount, nco::hasIMContact::iri(), imAddress));

        updateQuery.addInsertion(imContact, nie::contentLastModified::iri(), RDFVariable(datetime));

        addContactAliasInfoToQuery(updateQuery, imAddress, contactWrapper);
        addContactPresenceInfoToQuery(updateQuery, imAddress, contactWrapper);
        addContactCapabilitiesInfoToQuery(updateQuery, imAddress, contactWrapper);

        // TODO add avatar support
    }
    ::tracker()->executeQuery(updateQuery);

    Q_UNUSED(contactsRemoved);
    // TODO remove contacts in contactsRemoved from tracker
    // RDFSelect removalQuery;
    // foreach (CDTpContact *contactWrapper, contactsRemoved) {
    //
    // }
}

void CDTpStorage::syncAccountContact(CDTpAccount *accountWrapper,
        CDTpContact *contactWrapper, CDTpContact::Changes changes)
{
    Q_UNUSED(accountWrapper);

    RDFUpdate updateQuery;
    const RDFVariable imAddress(contactWrapper);

    if (changes & CDTpContact::Alias) {
        addContactAliasInfoToQuery(updateQuery, imAddress, contactWrapper);
    }
    if (changes & CDTpContact::Presence) {
        addContactPresenceInfoToQuery(updateQuery, imAddress, contactWrapper);
    }
    if (changes & CDTpContact::Capabilities) {
        addContactCapabilitiesInfoToQuery(updateQuery, imAddress, contactWrapper);
    }
    if (changes & CDTpContact::Avatar) {
        // TODO: add avatar support
    }

    ::tracker()->executeQuery(updateQuery);
}

void CDTpStorage::setAccountContactsOffline(CDTpAccount *accountWrapper)
{
    RDFUpdate updateQuery;
    foreach (CDTpContact *contactWrapper, accountWrapper->contacts()) {
        const RDFVariable imAddress(contactImAddress(contactWrapper));

        updateQuery.addDeletion(imAddress, nco::imPresence::iri());
        updateQuery.addDeletion(imAddress, nco::imStatusMessage::iri());
        updateQuery.addDeletion(imAddress, nco::presenceLastModified::iri());

        const QLatin1String status("unknown");
        updateQuery.addInsertion(RDFStatementList() <<
                RDFStatement(imAddress, nco::imStatusMessage::iri(),
                    LiteralValue("")) <<
                RDFStatement(imAddress, nco::imPresence::iri(),
                    trackerStatusFromTpPresenceStatus(status)));

        updateQuery.addInsertion(imAddress, nco::presenceLastModified::iri(),
                RDFVariable(QDateTime::currentDateTime()));
    }
    ::tracker()->executeQuery(updateQuery);
}

void CDTpStorage::removeAccount(CDTpAccount *accountWrapper)
{
    Tp::AccountPtr account = accountWrapper->account();
    QString accountObjectPath = account->objectPath();

    RDFVariable imContact = RDFVariable::fromType<nco::PersonContact>();
    RDFVariable imAddress = imContact.optional().property<nco::hasIMAddress>();
    RDFVariable imAccount = RDFVariable::fromType<nco::IMAccount>();
    imAccount.property<nco::hasIMContact>() = imAddress;

    RDFSelect select;
    select.addColumn("contact", imContact);
    select.addColumn("distinct", imAddress.property<nco::imID>());
    select.addColumn("contactId", imContact.property<nco::contactLocalUID>());
    select.addColumn("accountPath", imAccount);
    select.addColumn("address", imAddress);

    // TODO: improve query to only return contacts whose account object path is
    //       accountObjectPath
    LiveNodes ncoContacts = ::tracker()->modelQuery(select);
    for (int i = 0; i < ncoContacts->rowCount(); ++i) {
        QString contactAccountObjectPath =
            ncoContacts->index(i, 3).data().toString().split(":").value(1);

        if (contactAccountObjectPath == accountObjectPath) {
            Live<nco::PersonContact> imContact = 
                ncoContacts->liveResource<nco::PersonContact>(i, 0);
            imContact->remove();
            Live<nco::IMAddress> imAddress =
                ncoContacts->liveResource<nco::PersonContact>(i, 4);
            imAddress->remove();
        }
    }

    // TODO: also remove account
}

bool CDTpStorage::saveAccountAvatar(const QByteArray &data, const QString &mimeType,
        const QString &path, QString &fileName)
{
    Q_UNUSED(mimeType);

    if (data.isEmpty()) {
        // nothing to write, avatar is empty
        return false;
    }

    fileName = path + QString(QCryptographicHash::hash(data,
                QCryptographicHash::Sha1).toHex());
    qDebug() << "Saving account avatar to" << fileName;

    QFile avatarFile(fileName);
    if (!avatarFile.open(QIODevice::WriteOnly)) {
        qWarning() << "Unable to save account avatar: error opening avatar "
            "file" << fileName << "for writing";
        return false;
    }
    avatarFile.write(data);
    avatarFile.close();

    qDebug() << "Account avatar saved successfully";

    return true;
}

void CDTpStorage::addContactAliasInfoToQuery(RDFUpdate &query,
        const RDFVariable &imAddress,
        CDTpContact *contactWrapper)
{
    Tp::ContactPtr contact = contactWrapper->contact();

    query.addDeletion(imAddress, nco::imNickname::iri());

    query.addInsertion(RDFStatement(imAddress, nco::imNickname::iri(),
                LiteralValue(contact->alias())));
}

void CDTpStorage::addContactPresenceInfoToQuery(RDFUpdate &query,
        const RDFVariable &imAddress,
        CDTpContact *contactWrapper)
{
    Tp::ContactPtr contact = contactWrapper->contact();

    query.addDeletion(imAddress, nco::imPresence::iri());
    query.addDeletion(imAddress, nco::imStatusMessage::iri());
    query.addDeletion(imAddress, nco::presenceLastModified::iri());

    query.addInsertion(RDFStatementList() <<
            RDFStatement(imAddress, nco::imStatusMessage::iri(),
                LiteralValue(contact->presenceMessage())) <<
            RDFStatement(imAddress, nco::imPresence::iri(),
                trackerStatusFromTpPresenceType(contact->presenceType())) << 
            RDFStatement(imAddress, nco::presenceLastModified::iri(),
                RDFVariable(QDateTime::currentDateTime())));
}

void CDTpStorage::addContactCapabilitiesInfoToQuery(RDFUpdate &query,
        const RDFVariable &imAddress,
        CDTpContact *contactWrapper)
{
    Tp::ContactPtr contact = contactWrapper->contact();

    query.addDeletion(imAddress, nco::imCapability::iri());

    if (contact->capabilities()->supportsTextChats()) {
        query.addInsertion(RDFStatementList() <<
                RDFStatement(imAddress, nco::imCapability::iri(),
                    nco::im_capability_text_chat::iri()));
    }

    if (contact->capabilities()->supportsAudioCalls()) {
        query.addInsertion(RDFStatementList() <<
                RDFStatement(imAddress, nco::imCapability::iri(),
                    nco::im_capability_audio_calls::iri()));
    }

    if (contact->capabilities()->supportsVideoCalls()) {
        query.addInsertion(RDFStatementList() <<
                RDFStatement(imAddress, nco::imCapability::iri(),
                    nco::im_capability_video_calls::iri()));
    }
}

QString CDTpStorage::contactLocalId(const QString &contactAccountObjectPath,
        const QString &contactId) const
{
    return QString::number(qHash(QString("%1!%2")
                .arg(contactAccountObjectPath)
                .arg(contactId)));
}

QString CDTpStorage::contactLocalId(CDTpContact *contactWrapper) const
{
    CDTpAccount *accountWrapper = contactWrapper->accountWrapper();
    Tp::AccountPtr account = accountWrapper->account();
    Tp::ContactPtr contact = contactWrapper->contact();
    return contactLocalId(account->objectPath(), contact->id());
}

QUrl CDTpStorage::contactIri(const QString &contactLocalId) const
{
    return QUrl(QString("contact:%1").arg(contactLocalId));
}

QUrl CDTpStorage::contactIri(CDTpContact *contactWrapper) const
{
    return contactIri(contactLocalId(contactWrapper));
}

QUrl CDTpStorage::contactImAddress(const QString &contactAccountObjectPath,
        const QString &contactId) const
{
    return QUrl(QString("telepathy:%1!%2")
            .arg(contactAccountObjectPath)
            .arg(contactId));
}

QUrl CDTpStorage::contactImAddress(CDTpContact *contactWrapper) const
{
    CDTpAccount *accountWrapper = contactWrapper->accountWrapper();
    Tp::AccountPtr account = accountWrapper->account();
    Tp::ContactPtr contact = contactWrapper->contact();
    return contactImAddress(account->objectPath(), contact->id());
}

QUrl CDTpStorage::trackerStatusFromTpPresenceType(uint tpPresenceType) const
{
    switch (tpPresenceType) {
    case Tp::ConnectionPresenceTypeUnset:
        return nco::presence_status_unknown::iri();
    case Tp::ConnectionPresenceTypeOffline:
        return nco::presence_status_offline::iri();
    case Tp::ConnectionPresenceTypeAvailable:
        return nco::presence_status_available::iri();
    case Tp::ConnectionPresenceTypeAway:
        return nco::presence_status_away::iri();
    case Tp::ConnectionPresenceTypeExtendedAway:
        return nco::presence_status_extended_away::iri();
    case Tp::ConnectionPresenceTypeHidden:
        return nco::presence_status_hidden::iri();
    case Tp::ConnectionPresenceTypeBusy:
        return nco::presence_status_busy::iri();
    case Tp::ConnectionPresenceTypeUnknown:
        return nco::presence_status_unknown::iri();
    case Tp::ConnectionPresenceTypeError:
        return nco::presence_status_error::iri();
    default:
        qWarning() << "Unknown telepathy presence status" << tpPresenceType;
    }

    return nco::presence_status_error::iri();
}

QUrl CDTpStorage::trackerStatusFromTpPresenceStatus(
        const QString &tpPresenceStatus) const
{
    static QHash<QString, QUrl> mapping;
    if (mapping.isEmpty()) {
        mapping.insert("offline", nco::presence_status_offline::iri());
        mapping.insert("available", nco::presence_status_available::iri());
        mapping.insert("away", nco::presence_status_away::iri());
        mapping.insert("xa", nco::presence_status_extended_away::iri());
        mapping.insert("dnd", nco::presence_status_busy::iri());
        mapping.insert("busy", nco::presence_status_busy::iri());
        mapping.insert("hidden", nco::presence_status_hidden::iri());
        mapping.insert("unknown", nco::presence_status_unknown::iri());
    }

    QHash<QString, QUrl>::const_iterator i(mapping.constFind(tpPresenceStatus));
    if (i != mapping.end()) {
        return *i;
    }
    return nco::presence_status_error::iri();
}

void CDTpStorage::updateAvatar(RDFUpdate &query,
        const QUrl &url,
        const QUrl &fileName,
        bool deleteOnly)
{
    // We need deleteOnly to handle cases where the avatar image was removed from the account
    if (!fileName.isValid()) {
        return;
    }

    RDFVariable imAddress(url);
    RDFVariable dataObject(fileName);

    query.addDeletion(imAddress, nco::imAvatar::iri());
    query.addDeletion(dataObject, nie::DataObject::iri());

    if (!deleteOnly) {
        query.addInsertion(RDFStatement(dataObject, rdf::type::iri(), nie::DataObject::iri()));
        query.addInsertion(RDFStatement(imAddress, nco::imAvatar::iri(), dataObject));
    }
}
