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

#include <QtTracker/ontologies/nco.h>
#include <QtTracker/ontologies/nie.h>

#include <TelepathyQt4/AvatarData>
#include <TelepathyQt4/ContactCapabilities>
#include <TelepathyQt4/ConnectionCapabilities>

#include "cdtpstorage.h"

#define MAX_UPDATE_SIZE 999
#define MAX_REMOVE_SIZE 999

const QString CDTpStorage::defaultGraph = QLatin1String("urn:uuid:08070f5c-a334-4d19-a8b0-12a3071bfab9");
const QString CDTpStorage::privateGraph = QLatin1String("urn:uuid:679293d4-60f0-49c7-8d63-f1528fe31f66");
const QString CDTpStorage::literalDefaultGraph = QString(QLatin1String("<%1>")).arg(defaultGraph);
const QString CDTpStorage::literalPrivateGraph = QString(QLatin1String("<%1>")).arg(privateGraph);

const QString defaultGenerator = "telepathy";

CDTpStorage::CDTpStorage(QObject *parent)
    : QObject(parent)
{
    //::tracker()->setServiceAttribute("tracker_access_method", QString("QSPARQL_DIRECT"));

    mQueueTimer.setSingleShot(true);
    mQueueTimer.setInterval(100);
    connect(&mQueueTimer, SIGNAL(timeout()), SLOT(onQueueTimerTimeout()));
}

CDTpStorage::~CDTpStorage()
{
}

void CDTpStorage::syncAccountSet(const QList<QString> &accounts)
{
    RDFVariable imAccount = RDFVariable::fromType<nco::IMAccount>();

    RDFVariableList members;
    Q_FOREACH (QString accountObjectPath, accounts) {
        members << RDFVariable(QUrl(QString("telepathy:%1").arg(accountObjectPath)));
    }
    imAccount.isMemberOf(members).not_();

    RDFSelect select;
    select.addColumn("Accounts", imAccount);

    CDTpSelectQuery *query = new CDTpSelectQuery(select, this);
    connect(query,
            SIGNAL(finished(CDTpSelectQuery *)),
            SLOT(onAccountPurgeSelectQueryFinished(CDTpSelectQuery *)));
}

void CDTpStorage::onAccountPurgeSelectQueryFinished(CDTpSelectQuery *query)
{
    LiveNodes result = query->reply();

    for (int i = 0; i < result->rowCount(); i++) {
        const QString accountUrl = result->index(i, 0).data().toString();
        const QString accountObjectPath = accountUrl.mid(QString("telepathy:").length());

        removeAccount(accountObjectPath);
    }
}

void CDTpStorage::syncAccount(CDTpAccountPtr accountWrapper)
{
    syncAccount(accountWrapper, CDTpAccount::All);

    /* If contactsd leaves while account is still online, and get restarted
     * when account is offline then contacts in tracker still have presence.
     * This happens when rebooting the device. */
    if (!accountWrapper->account()->connection()) {
        setAccountContactsOffline(accountWrapper);
    }
}

// TODO: Improve syncAccount so that it only updates the data that really
//       changed
void CDTpStorage::syncAccount(CDTpAccountPtr accountWrapper,
        CDTpAccount::Changes changes)
{
    Tp::AccountPtr account = accountWrapper->account();
    if (account->normalizedName().isEmpty()) {
        return;
    }

    const QString accountObjectPath = account->objectPath();
    const QString accountId = account->normalizedName();
    const QDateTime datetime = QDateTime::currentDateTime();

    qDebug() << "Syncing account" << accountObjectPath << "to storage" << accountId;

    RDFUpdate up;
    RDFStatementList inserts;

    // Create the IMAddress for this account's self contact
    RDFVariable imAddress(contactImAddress(accountObjectPath, accountId));
    inserts << RDFStatement(imAddress, rdf::type::iri(), nco::IMAddress::iri())
            << RDFStatement(imAddress, nco::imID::iri(), LiteralValue(account->normalizedName()))
            << RDFStatement(imAddress, nco::imProtocol::iri(), LiteralValue(account->protocolName()));

    if (changes & CDTpAccount::Nickname) {
        qDebug() << "  nickname changed";
        up.addDeletion(imAddress, nco::imNickname::iri(), RDFVariable(), QUrl(defaultGraph));
        inserts << RDFStatement(imAddress, nco::imNickname::iri(), LiteralValue(account->nickname()));
    }

    if (changes & CDTpAccount::Presence) {
        qDebug() << "  presence changed";
        Tp::Presence presence = account->currentPresence();
        QUrl status = trackerStatusFromTpPresenceStatus(presence.status());

        up.addDeletion(imAddress, nco::imPresence::iri(), RDFVariable(), QUrl(defaultGraph));
        up.addDeletion(imAddress, nco::imStatusMessage::iri(), RDFVariable(), QUrl(defaultGraph));
        up.addDeletion(imAddress, nco::presenceLastModified::iri(), RDFVariable(), QUrl(defaultGraph));

        inserts << RDFStatement(imAddress, nco::imPresence::iri(), RDFVariable(status))
                << RDFStatement(imAddress, nco::imStatusMessage::iri(), LiteralValue(presence.statusMessage()))
                << RDFStatement(imAddress, nco::presenceLastModified::iri(), LiteralValue(datetime));
    }

    if (changes & CDTpAccount::Avatar) {
        qDebug() << "  avatar changed";
        const Tp::Avatar &avatar = account->avatar();
        // TODO: saving to disk needs to be removed here
        saveAccountAvatar(up, avatar.avatarData, avatar.MIMEType, imAddress,
            inserts);
    }

    // Create an IMAccount
    RDFStatementList imAccountInserts;
    RDFVariable imAccount(QUrl(QString("telepathy:%1").arg(accountObjectPath)));
    imAccountInserts << RDFStatement(imAccount, rdf::type::iri(), nco::IMAccount::iri())
                     << RDFStatement(imAccount, nco::imAccountType::iri(), LiteralValue(account->protocolName()))
                     << RDFStatement(imAccount, nco::imAccountAddress::iri(), imAddress)
                     << RDFStatement(imAccount, nco::hasIMContact::iri(), imAddress);

    if (changes & CDTpAccount::DisplayName) {
        qDebug() << "  display name changed";
        up.addDeletion(imAccount, nco::imDisplayName::iri(), RDFVariable(), QUrl(privateGraph));
        imAccountInserts << RDFStatement(imAccount, nco::imDisplayName::iri(), LiteralValue(account->displayName()));
    }

    // link the IMAddress to me-contact via an affiliation
    const QString strLocalUID = QString::number(0x7FFFFFFF);
    RDFVariable imAffiliation(contactAffiliation(accountObjectPath, accountId));
    inserts << RDFStatement(imAffiliation, rdf::type::iri(), nco::Affiliation::iri())
            << RDFStatement(imAffiliation, rdfs::label::iri(), LiteralValue("Other"))
            << RDFStatement(imAffiliation, nco::hasIMAddress::iri(), imAddress);
    inserts << RDFStatement(nco::default_contact_me::iri(), nco::hasAffiliation::iri(), imAffiliation)
            << RDFStatement(nco::default_contact_me::iri(), nco::contactUID::iri(), LiteralValue(strLocalUID))
            << RDFStatement(nco::default_contact_me::iri(), nco::contactLocalUID::iri(), LiteralValue(strLocalUID))
            << RDFStatement(nco::default_contact_me::iri(), nie::contentLastModified::iri(), LiteralValue(datetime));
    up.addDeletion(nco::default_contact_me::iri(), nie::contentLastModified::iri(), RDFVariable(), QUrl(defaultGraph));

    up.addInsertion(inserts, QUrl(defaultGraph));
    up.addInsertion(imAccountInserts, QUrl(privateGraph));
    new CDTpUpdateQuery(up);
}

void CDTpStorage::saveAccountAvatar(RDFUpdate &query, const QByteArray &data, const QString &mimeType,
        const RDFVariable &imAddress, RDFStatementList &inserts)
{
    Q_UNUSED(mimeType);

    query.addDeletion(imAddress, nco::imAvatar::iri(), RDFVariable(), QUrl(defaultGraph));

    if (data.isEmpty()) {
        return;
    }

    QString fileName = QString("%1/.contacts/avatars/%2")
        .arg(QDir::homePath())
        .arg(QString(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex()));
    qDebug() << "Saving account avatar to" << fileName;

    QFile avatarFile(fileName);
    if (!avatarFile.open(QIODevice::WriteOnly)) {
        qWarning() << "Unable to save account avatar: error opening avatar "
            "file" << fileName << "for writing";
        return;
    }
    avatarFile.write(data);
    avatarFile.close();

    RDFVariable dataObject(QUrl::fromLocalFile(fileName));
    query.addDeletion(dataObject, nie::url::iri(), RDFVariable(), QUrl(defaultGraph));

    inserts << RDFStatement(dataObject, rdf::type::iri(), nie::DataObject::iri())
            << RDFStatement(dataObject, nie::url::iri(), dataObject)
            << RDFStatement(imAddress, nco::imAvatar::iri(), dataObject);
}

void CDTpStorage::syncAccountContacts(CDTpAccountPtr accountWrapper)
{
    CDTpStorageSyncOperations &op = mSyncOperations[accountWrapper];
    if (!op.active) {
        op.active = true;
        Q_EMIT syncStarted(accountWrapper);
    }

    syncAccountContacts(accountWrapper, accountWrapper->contacts(),
            QList<CDTpContactPtr>());

    /* We need to purge contacts that are in tracker but got removed from the
     * account while contactsd was not running.
     * We first query all contacts for that account, then will delete those that
     * are not in the account anymore. We can't use NOT IN() in the query
     * because with huge contact list it will hit SQL limit. */
    op.nPendingOperations++;
    CDTpAccountContactsSelectQuery *query = new CDTpAccountContactsSelectQuery(accountWrapper, this);
    connect(query,
            SIGNAL(finished(CDTpSelectQuery *)),
            SLOT(onContactPurgeSelectQueryFinished(CDTpSelectQuery *)));
}

void CDTpStorage::syncAccountContacts(CDTpAccountPtr accountWrapper,
        const QList<CDTpContactPtr> &contactsAdded,
        const QList<CDTpContactPtr> &contactsRemoved)
{
    Q_FOREACH (CDTpContactPtr contactWrapper, contactsAdded) {
        queueContactUpdate(contactWrapper, CDTpContact::All);
    }

    removeContacts(accountWrapper, contactsRemoved);
}

void CDTpStorage::syncAccountContact(CDTpAccountPtr accountWrapper,
        CDTpContactPtr contactWrapper, CDTpContact::Changes changes)
{
    Q_UNUSED(accountWrapper);
    queueContactUpdate(contactWrapper, changes);
}

void CDTpStorage::setAccountContactsOffline(CDTpAccountPtr accountWrapper)
{
    qDebug() << "Setting presence to UNKNOWN for all contacts of account"
             << accountWrapper->account()->objectPath();

    const QString accountId = accountWrapper->account()->normalizedName();
    const QString accountPath = accountWrapper->account()->objectPath();
    const QString selfIMAddress = literalIMAddress(accountPath, accountId);

    // Select all imAddress and their imContact, except for the self contact,
    // because we know that self contact has presence Offline, and that's
    // already done in SyncAccount().
    CDTpStorageBuilder builder;
    builder.addCustomSelection(QString(QLatin1String("FILTER(?imAddress != %1)")).arg(selfIMAddress));
    builder.addCustomSelection(QString(QLatin1String("<telepathy:%1> nco:hasIMContact ?imAddress")).arg(accountPath));
    builder.addCustomSelection(QString(QLatin1String("?imContact nco:hasAffiliation [ nco:hasIMAddress ?imAddress ]")));
    builder.updateProperty("?imAddress", "nco:imPresence", "nco:presence-status-unknown");
    builder.updateProperty("?imAddress", "nco:presenceLastModified", literalTimeStamp());
    builder.updateProperty("?imContact", "nie:contentLastModified", literalTimeStamp());

    // Update capabilities of all contacts, since we don't know them anymore,
    // reset them to the account's caps.
    addContactCapabilitiesToBuilder(builder, "?imAddress",
            accountWrapper->account()->capabilities());

    new CDTpSparqlQuery(builder.getSparqlQuery(), this);
}

void CDTpStorage::queueContactUpdate(CDTpContactPtr contactWrapper, CDTpContact::Changes changes)
{
    if (!mUpdateQueue.contains(contactWrapper)) {
        qDebug() << "queue update for" << contactWrapper->contact()->id();
        mUpdateQueue.insert(contactWrapper, changes);
        mSyncOperations[contactWrapper->accountWrapper()].nPendingOperations++;
    } else {
        mUpdateQueue[contactWrapper] |= changes;
    }

    /* If the queue is too big, flush it now to avoid hitting query size limit */
    if (mUpdateQueue.size() >= MAX_UPDATE_SIZE) {
        onQueueTimerTimeout();
    } else if (!mQueueTimer.isActive()) {
        mQueueTimer.start();
    }
}

void CDTpStorage::onQueueTimerTimeout()
{
    if (!mUpdateQueue.isEmpty()) {
        updateQueuedContacts();
        mUpdateQueue.clear();
    }
}

void CDTpStorage::updateQueuedContacts()
{
    QString finalQuery;

    // Ensure all imAddress exists and are linked from imAccount
    CDTpStorageBuilder builder;
    QStringList imAddresses;
    Q_FOREACH (const CDTpContactPtr contactWrapper, mUpdateQueue.keys()) {
        CDTpAccountPtr accountWrapper = contactWrapper->accountWrapper();

        const QString imAddress = literalIMAddress(contactWrapper);
        builder.createResource(imAddress, "nco:IMAddress");
        builder.insertProperty(imAddress, "nco:imID", literal(contactWrapper->contact()->id()));
        builder.insertProperty(imAddress, "nco:imProtocol", literal(accountWrapper->account()->protocolName()));

        const QString imAccount = literalIMAccount(accountWrapper);
        builder.insertProperty(imAccount, "nco:hasIMContact", imAddress);

        imAddresses << imAddress;
    }
    finalQuery += builder.getRawQuery();
    builder.reset();

    // Ensure all imAddresses are bound to a PersonContact
    finalQuery += QString(QLatin1String(
        "INSERT {\n"
        "    GRAPH %1 {\n"
        "        _:contact a nco:PersonContact;\n"
        "                  nco:hasAffiliation _:affiliation;\n"
        "                  nie:contentCreated %2;\n"
        "                  nie:generator %3 .\n"
        "        _:affiliation a nco:Affiliation;\n"
        "                      nco:hasIMAddress ?imAddress;\n"
        "                      rdfs:label \"Others\".\n"
        "    }\n"
        "}\n"
        "WHERE {\n"
        "    ?imAddress a nco:IMAddress.\n"
        "    FILTER (?imAddress IN (%4) &&\n"
        "            NOT EXISTS { ?contact nco:hasAffiliation [ nco:hasIMAddress ?imAddress ] })\n"
        "}\n\n"))
        .arg(literalDefaultGraph).arg(literalTimeStamp())
        .arg(literal(defaultGenerator)).arg(imAddresses.join(QLatin1String(",")));

    // Update all contacts...
    QHash<CDTpContactPtr, CDTpContact::Changes>::const_iterator i;
    for (i = mUpdateQueue.constBegin(); i != mUpdateQueue.constEnd(); ++i) {
        const CDTpContactPtr contactWrapper = i.key();
        const CDTpAccountPtr accountWrapper = contactWrapper->accountWrapper();
        const Tp::ContactPtr contact = contactWrapper->contact();

        const QString accountPath = accountWrapper->account()->objectPath();
        const QString contactId = contact->id();
        const QString imAddress = literalIMAddress(accountPath, contactId);

        // bind imContact to imAddress
        const QString imContact = builder.uniquify("?imContact");
        builder.addCustomSelection(QString(QLatin1String(
                "%1 nco:hasAffiliation [ nco:hasIMAddress %2 ]"))
                .arg(imContact).arg(imAddress));

        // Update contact's timestamp
        builder.updateProperty(imContact, "nie:contentLastModified", literalTimeStamp());

        // Apply all changes
        const CDTpContact::Changes changes = i.value();
        if (changes & CDTpContact::Alias) {
            qDebug() << "  alias changed";
            addContactAliasToBuilder(builder, imAddress, contactWrapper);
        }
        if (changes & CDTpContact::Presence) {
            qDebug() << "  presence changed";
            addContactPresenceToBuilder(builder, imAddress, contactWrapper);
        }
        if (changes & CDTpContact::Capabilities) {
            qDebug() << "  capabilities changed";
            addContactCapabilitiesToBuilder(builder, imAddress, contact->capabilities());
        }
        if (changes & CDTpContact::Avatar) {
            qDebug() << "  avatar changed";
            addContactAvatarToBuilder(builder, imAddress, contactWrapper);
        }
        if (changes & CDTpContact::Authorization) {
            qDebug() << "  authorization changed";
            addContactAuthorizationToBuilder(builder, imAddress, contactWrapper);
        }
        if (changes & CDTpContact::Infomation) {
            qDebug() << "  vcard information changed";
            addContactInfoToBuilder(builder, imAddress, imContact, contactWrapper);
        }
    }
    finalQuery += builder.getRawQuery();

    /* Keep only one sync operation per account instead of per contact */
    QList<CDTpAccountPtr> accounts;
    Q_FOREACH (const CDTpContactPtr contactWrapper, mUpdateQueue.keys()) {
        CDTpAccountPtr accountWrapper = contactWrapper->accountWrapper();
        if (!accounts.contains(accountWrapper)) {
            accounts << accountWrapper;
        } else {
            oneSyncOperationFinished(accountWrapper);
        }
    }

    CDTpAccountsSparqlQuery *query = new CDTpAccountsSparqlQuery(accounts,
            QSparqlQuery(finalQuery, QSparqlQuery::InsertStatement));
    connect(query,
            SIGNAL(finished(CDTpSparqlQuery *)),
            SLOT(onAccountsSparqlQueryFinished(CDTpSparqlQuery *)));
}

void CDTpStorage::addContactAliasToBuilder(CDTpStorageBuilder &builder,
        const QString &imAddress,
        CDTpContactPtr contactWrapper) const
{
    Tp::ContactPtr contact = contactWrapper->contact();
    builder.updateProperty(imAddress, "nco:imNickname", literal(contact->alias()));
}

void CDTpStorage::addContactPresenceToBuilder(CDTpStorageBuilder &builder,
        const QString &imAddress,
        CDTpContactPtr contactWrapper) const
{
    Tp::ContactPtr contact = contactWrapper->contact();
    builder.updateProperty(imAddress, "nco:imPresence", presenceType(contact->presence().type()));
    builder.updateProperty(imAddress, "nco:imStatusMessage", literal(contact->presence().statusMessage()));
    builder.updateProperty(imAddress, "nco:presenceLastModified", literalTimeStamp());
}

void CDTpStorage::addContactCapabilitiesToBuilder(CDTpStorageBuilder &builder,
        const QString &imAddress,
        Tp::CapabilitiesBase capabilities) const
{
    builder.deleteProperty(imAddress, "nco:imCapability");

    if (capabilities.textChats()) {
        builder.insertProperty(imAddress, "nco:imCapability", "nco:im-capability-text-chat");
    }
    if (capabilities.streamedMediaAudioCalls()) {
        builder.insertProperty(imAddress, "nco:imCapability", "nco:im-capability-audio-calls");
    }
    if (capabilities.streamedMediaVideoCalls()) {
        builder.insertProperty(imAddress, "nco:imCapability", "nco:im-capability-video-calls");
    }
}

void CDTpStorage::addContactAvatarToBuilder(CDTpStorageBuilder &builder,
        const QString &imAddress,
        CDTpContactPtr contactWrapper) const
{
    Tp::ContactPtr contact = contactWrapper->contact();

    /* If we don't know the avatar token, it is preferable to keep the old
     * avatar until we get an update. */
    if (!contact->isAvatarTokenKnown()) {
        return;
    }

    /* If we have a token but not an avatar filename, that probably means the
     * avatar data is being requested and we'll get an update later. */
    if (!contact->avatarToken().isEmpty() &&
        contact->avatarData().fileName.isEmpty()) {
        return;
    }

    /* Remove current avatar */
    /* FIXME: also delete the dataObject */
    builder.deleteProperty(imAddress, "nco:imAvatar");

    /* Insert new avatar */
    if (!contact->avatarToken().isEmpty()) {
        const QString dataObject = builder.uniquify("_:dataObject");
        builder.createResource(dataObject, "nie:DataObject");
        builder.insertProperty(dataObject, "nie:url", literal(contact->avatarData().fileName));
        builder.insertProperty(imAddress, "nco:imAvatar", dataObject);
    }
}

void CDTpStorage::addContactAuthorizationToBuilder(CDTpStorageBuilder &builder,
        const QString &imAddress,
        CDTpContactPtr contactWrapper) const
{
    Tp::ContactPtr contact = contactWrapper->contact();
    builder.updateProperty(imAddress, "nco:imAddressAuthStatusFrom", presenceState(contact->subscriptionState()));
    builder.updateProperty(imAddress, "nco:imAddressAuthStatusTo", presenceState(contact->publishState()));
}

void CDTpStorage::addContactInfoToBuilder(CDTpStorageBuilder &builder,
        const QString &imAddress,
        const QString &imContact,
        CDTpContactPtr contactWrapper) const
{
    /* Use the imAddress as graph for ContactInfo fields, so we can easilly
     * know from which contact it comes from */
    const QString graph = imAddress;

    /* Drop current info */
    addRemoveContactInfoToBuilder(builder, imContact, graph);

    Tp::ContactPtr contact = contactWrapper->contact();
    Tp::ContactInfoFieldList listContactInfo = contact->infoFields().allFields();
    if (listContactInfo.count() == 0) {
        qDebug() << "No contact info present";
        return;
    }

    QHash<QString, QString> affiliations;
    Q_FOREACH (const Tp::ContactInfoField &field, listContactInfo) {
        if (field.fieldValue.count() == 0) {
            continue;
        }

        /* FIXME:
         *  - Do we care about "fn" and "nickname" ?
         *  - How do we write affiliation for "org" ?
         */

        if (!field.fieldName.compare("tel")) {
            const QString affiliation = ensureContactAffiliationToBuilder(builder, imContact, graph, field, affiliations);
            const QString voicePhoneNumber = builder.uniquify("_:tel");
            builder.createResource(voicePhoneNumber, "nco:VoicePhoneNumber", graph);
            builder.insertProperty(voicePhoneNumber, "maemo:localPhoneNumber", literalContactInfo(field, 0), graph);
            builder.insertProperty(voicePhoneNumber, "nco:phoneNumber", literalContactInfo(field, 0), graph);
            builder.insertProperty(affiliation, "nco:hasPhoneNumber", voicePhoneNumber, graph);
        }

        else if (!field.fieldName.compare("adr")) {
            const QString affiliation = ensureContactAffiliationToBuilder(builder, imContact, graph, field, affiliations);
            const QString postalAddress = builder.uniquify("_:address");
            builder.createResource(postalAddress, "nco:PostalAddress", graph);
            builder.insertProperty(postalAddress, "nco:pobox",           literalContactInfo(field, 0), graph);
            builder.insertProperty(postalAddress, "nco:extendedAddress", literalContactInfo(field, 1), graph);
            builder.insertProperty(postalAddress, "nco:streetAddress",   literalContactInfo(field, 2), graph);
            builder.insertProperty(postalAddress, "nco:locality",        literalContactInfo(field, 3), graph);
            builder.insertProperty(postalAddress, "nco:region",          literalContactInfo(field, 4), graph);
            builder.insertProperty(postalAddress, "nco:postalcode",      literalContactInfo(field, 5), graph);
            builder.insertProperty(postalAddress, "nco:country",         literalContactInfo(field, 6), graph);
            builder.insertProperty(affiliation, "nco:hasPostalAddress", postalAddress, graph);
        }

        else if (!field.fieldName.compare("email")) {
            const QString affiliation = ensureContactAffiliationToBuilder(builder, imContact, graph, field, affiliations);
            const QString emailAddress = builder.uniquify("_:email");
            builder.createResource(emailAddress, "nco:EmailAddress", graph);
            builder.insertProperty(emailAddress, "nco:emailAddress", literalContactInfo(field, 0), graph);
        }

        else if (!field.fieldName.compare("url")) {
            const QString affiliation = ensureContactAffiliationToBuilder(builder, imContact, graph, field, affiliations);
            builder.insertProperty(affiliation, "nco:url", literalContactInfo(field, 0), graph);
        }

        else if (!field.fieldName.compare("title")) {
            const QString affiliation = ensureContactAffiliationToBuilder(builder, imContact, graph, field, affiliations);
            builder.insertProperty(affiliation, "nco:title", literalContactInfo(field, 0), graph);
        }

        else if (!field.fieldName.compare("role")) {
            const QString affiliation = ensureContactAffiliationToBuilder(builder, imContact, graph, field, affiliations);
            builder.insertProperty(affiliation, "nco:role", literalContactInfo(field, 0), graph);
        }

        else if (!field.fieldName.compare("note") || !field.fieldName.compare("desc")) {
            builder.insertProperty(imContact, "nco:note", literalContactInfo(field, 0), graph);
        }

        else if (!field.fieldName.compare("bday")) {
            /* Tracker will reject anything not [-]CCYY-MM-DDThh:mm:ss[Z|(+|-)hh:mm]
             * VCard spec allows only ISO 8601, but most IM clients allows
             * any string. */
            /* FIXME: support more date format for compatibility */
            QDate date = QDate::fromString(field.fieldValue[0], "yyyy-MM-dd");
            if (!date.isValid()) {
                date = QDate::fromString(field.fieldValue[0], "yyyyMMdd");
            }

            if (date.isValid()) {
                builder.insertProperty(imContact, "nco:birthDate", literal(QDateTime(date)), graph);
            } else {
                qDebug() << "Unsupported bday format:" << field.fieldValue[0];
            }
        }

        else {
            qDebug() << "Unsupported VCard field" << field.fieldName;
        }
    }
}

QString CDTpStorage::ensureContactAffiliationToBuilder(CDTpStorageBuilder &builder,
        const QString &imContact,
        const QString &graph,
        const Tp::ContactInfoField &field,
        QHash<QString, QString> &affiliations) const
{
    static QHash<QString, QString> knownTypes;
    if (knownTypes.isEmpty()) {
        knownTypes.insert ("work", "Work");
        knownTypes.insert ("home", "Home");
    }

    QString type = "Other";
    Q_FOREACH (const QString &parameter, field.parameters) {
        if (!parameter.startsWith("type=")) {
            continue;
        }

        const QString str = parameter.mid(5);
        if (knownTypes.contains(str)) {
            type = knownTypes[str];
            break;
        }
    }

    if (!affiliations.contains(type)) {
        const QString affiliation = builder.uniquify("_:affiliation");
        builder.createResource(affiliation, "nco:Affiliation", graph);
        builder.insertProperty(affiliation, "rdfs:label", literal(type), graph);
        builder.insertProperty(imContact, "nco:hasAffiliation", affiliation, graph);
        affiliations.insert(type, affiliation);
    }

    return affiliations[type];
}

void CDTpStorage::addRemoveContactInfoToBuilder(CDTpStorageBuilder &builder,
        const QString &imContact,
        const QString &graph) const
{
    builder.deleteProperty(imContact, "nco:birthDate", graph);
    builder.deleteProperty(imContact, "nco:note", graph);

    /* Remove affiliation and its sub resources */
    const QString affiliation = builder.deleteProperty(imContact, "nco:hasAffiliation", graph);
    builder.deletePropertyAndLinkedResource(affiliation, "nco:hasPhoneNumber");
    builder.deletePropertyAndLinkedResource(affiliation, "nco:hasPostalAddress");
    builder.deletePropertyAndLinkedResource(affiliation, "nco:emailAddress");
    builder.deleteResource(affiliation);
}

void CDTpStorage::onAccountsSparqlQueryFinished(CDTpSparqlQuery *query)
{
    CDTpAccountsSparqlQuery *accountsQuery = qobject_cast<CDTpAccountsSparqlQuery *>(query);
    Q_FOREACH (const CDTpAccountPtr &accountWrapper, accountsQuery->accounts()) {
        oneSyncOperationFinished(accountWrapper);
    }
}

void CDTpStorage::oneSyncOperationFinished(CDTpAccountPtr accountWrapper)
{
    CDTpStorageSyncOperations &op = mSyncOperations[accountWrapper];
    op.nPendingOperations--;

    if (op.nPendingOperations == 0) {
        if (op.active) {
            Q_EMIT syncEnded(accountWrapper, op.nContactsAdded, op.nContactsRemoved);
        }
        mSyncOperations.remove(accountWrapper);
    }
}

QString CDTpStorage::presenceType(Tp::ConnectionPresenceType presenceType) const
{
    switch (presenceType) {
    case Tp::ConnectionPresenceTypeUnset:
        return QString("nco:presence-status-unknown");
    case Tp::ConnectionPresenceTypeOffline:
        return QString("nco:presence-status-offline");
    case Tp::ConnectionPresenceTypeAvailable:
        return QString("nco:presence-status-available");
    case Tp::ConnectionPresenceTypeAway:
        return QString("nco:presence-status-away");
    case Tp::ConnectionPresenceTypeExtendedAway:
        return QString("nco:presence-status-extended-away");
    case Tp::ConnectionPresenceTypeHidden:
        return QString("nco:presence-status-hidden");
    case Tp::ConnectionPresenceTypeBusy:
        return QString("nco:presence-status-busy");
    case Tp::ConnectionPresenceTypeUnknown:
        return QString("nco:presence-status-unknown");
    case Tp::ConnectionPresenceTypeError:
        return QString("nco:presence-status-error");
    default:
        break;
    }

    qWarning() << "Unknown telepathy presence status" << presenceType;

    return QString("nco:presence-status-error");
}

QString CDTpStorage::presenceState(Tp::Contact::PresenceState presenceState) const
{
    switch (presenceState) {
    case Tp::Contact::PresenceStateNo:
        return QString("nco:predefined-auth-status-no");
    case Tp::Contact::PresenceStateAsk:
        return QString("nco:predefined-auth-status-requested");
    case Tp::Contact::PresenceStateYes:
        return QString("nco:predefined-auth-status-yes");
    }

    qWarning() << "Unknown telepathy presence state:" << presenceState;

    return QString("nco:predefined-auth-status-no");
}

// Copied from cubi/src/literalvalue.cpp
static QString
sparqlEscape(const QString &original)
{
	QString escaped;
	escaped.reserve(original.size());

	for(int i = 0; i < original.size(); ++i) {
		switch(original.at(i).toAscii()) {
			case '\t':
				escaped.append("\\t");
				break;
			case '\n':
				escaped.append("\\n");
				break;
			case '\r':
				escaped.append("\\r");
				break;
			case '"':
				escaped.append("\\\"");
				break;
			case '\\':
				escaped.append("\\\\");
				break;
			default:
				escaped.append(original.at(i));
		}
	}

	return escaped;
}

QString CDTpStorage::literal(const QString &str) const
{
    static const QString stringTemplate = QString::fromLatin1("\"%1\"");
    return stringTemplate.arg(sparqlEscape(str));
}

// Copied from cubi/src/literalvalue.cpp
QString CDTpStorage::literal(const QDateTime &dateTimeValue) const
{
    static const QString utcTimeTemplate = QString::fromLatin1("%1Z");

    // This is a workaround for http://bugreports.qt.nokia.com/browse/QTBUG-9698
    return literal(dateTimeValue.timeSpec() == Qt::UTC ?
            utcTimeTemplate.arg(dateTimeValue.toString(Qt::ISODate))
            : dateTimeValue.toString(Qt::ISODate));
}

QString CDTpStorage::literalTimeStamp() const
{
    return literal(QDateTime::currentDateTime());
}

QString CDTpStorage::literalIMAddress(const QString &accountPath, const QString &contactId) const
{
    return QString("<telepathy:%1!%2>").arg(accountPath).arg(contactId);
}

QString CDTpStorage::literalIMAddress(const CDTpContactPtr &contactWrapper) const
{
    const QString accountPath = contactWrapper->accountWrapper()->account()->objectPath();
    const QString contactId = contactWrapper->contact()->id();
    return literalIMAddress(accountPath, contactId);
}

QString CDTpStorage::literalIMAccount(const CDTpAccountPtr &accountWrapper) const
{
    const QString accountPath = accountWrapper->account()->objectPath();
    return QString("<telepathy:%1>").arg(accountPath);
}

QString CDTpStorage::literalContactInfo(const Tp::ContactInfoField &field, int i) const
{
    if (i >= field.fieldValue.count()) {
        return literal(QString());
    }

    return literal(field.fieldValue[i]);
}

void CDTpStorage::addRemoveContactToQuery(RDFUpdate &query,
        RDFStatementList &inserts,
        RDFStatementList &deletions,
        const CDTpContactsSelectItem &item)
{
    const QUrl imContact(item.imContact);
    const QUrl imAffiliation(item.imAffiliation);
    const QUrl imAddress(item.imAddress);
    bool deleteIMContact = (item.generator == defaultGenerator);

    qDebug() << "Deleting" << imAddress << "from" << imContact;
    qDebug() << "Also delete local contact:" << (deleteIMContact ? "Yes" : "No");

    /* Drop the imAddress and its affiliation */
    deletions << RDFStatement(imAffiliation, rdf::type::iri(), rdfs::Resource::iri());
    deletions << RDFStatement(imAddress, rdf::type::iri(), rdfs::Resource::iri());

    if (deleteIMContact) {
        /* The PersonContact is still owned by contactsd, drop it entirely */
        deletions << RDFStatement(imContact, rdf::type::iri(), rdfs::Resource::iri());
    } else {
        /* The PersonContact got modified by someone else, drop only the
         * hasAffiliation and keep the local contact in case it contains
         * additional info */
        deletions << RDFStatement(imContact, nco::hasAffiliation::iri(), imAffiliation);

        /* Update last modified time */
        const QDateTime datetime = QDateTime::currentDateTime();
        deletions << RDFStatement(imContact, nie::contentLastModified::iri(), RDFVariable());
        inserts << RDFStatement(imContact, nie::contentLastModified::iri(), LiteralValue(datetime));
    }

    addRemoveContactInfoToQuery(query, imContact, imAddress);
}

void CDTpStorage::addRemoveContactFromAccountToQuery(RDFStatementList &deletions,
        const CDTpContactsSelectItem &item)
{
    const QUrl imAddress(item.imAddress);
    const QUrl imAccount(QUrl(item.imAddress.left(item.imAddress.indexOf("!"))));
    deletions << RDFStatement(imAccount, nco::hasIMContact::iri(), imAddress);
}

void CDTpStorage::removeAccount(const QString &accountObjectPath)
{
    qDebug() << "Removing account" << accountObjectPath << "from storage";

    /* Delete the imAccount resource */
    RDFUpdate updateQuery;
    const RDFVariable imAccount(QUrl(QString("telepathy:%1").arg(accountObjectPath)));
    updateQuery.addDeletion(imAccount, rdf::type::iri(), rdfs::Resource::iri(), QUrl(privateGraph));
    new CDTpUpdateQuery(updateQuery);

    /* Delete all imAddress from that account */
    CDTpContactsSelectQuery *query = new CDTpContactsSelectQuery(accountObjectPath, this);
    connect(query,
            SIGNAL(finished(CDTpSelectQuery *)),
            SLOT(onAccountDeleteSelectQueryFinished(CDTpSelectQuery *)));
}

void CDTpStorage::removeContacts(CDTpAccountPtr accountWrapper,
        const QList<CDTpContactPtr> &contacts)
{
    /* Split the request into smaller batches if necessary */
    if (contacts.size() > MAX_REMOVE_SIZE) {
        QList<CDTpContactPtr> batch;
        for (int i = 0; i < contacts.size(); i++) {
            batch << contacts[i];
            if (batch.size() == MAX_REMOVE_SIZE) {
                removeContacts(accountWrapper, batch);
                batch.clear();
            }
        }
        if (!batch.isEmpty()) {
            removeContacts(accountWrapper, batch);
            batch.clear();
        }
        return;
    }

    /* Cancel queued update if we are going to remove the contact anyway */
    Q_FOREACH (const CDTpContactPtr &contactWrapper, contacts) {
        qDebug() << "Contact Removed, cancel update:" << contactWrapper->contact()->id();
        mUpdateQueue.remove(contactWrapper);
    }

    CDTpContactsSelectQuery *query = new CDTpContactsSelectQuery(contacts, this);
    connect(query,
            SIGNAL(finished(CDTpSelectQuery *)),
            SLOT(onContactDeleteSelectQueryFinished(CDTpSelectQuery *)));
}

void CDTpStorage::onContactPurgeSelectQueryFinished(CDTpSelectQuery *query)
{
    CDTpAccountContactsSelectQuery *contactsQuery =
        qobject_cast<CDTpAccountContactsSelectQuery*>(query);
    CDTpAccountPtr accountWrapper = contactsQuery->accountWrapper();

    LiveNodes result = query->reply();
    if (result->rowCount() <= 0) {
        oneSyncOperationFinished(accountWrapper);
        return;
    }

    RDFUpdate updateQuery;
    RDFStatementList deletions;
    RDFStatementList accountDeletions;
    RDFStatementList inserts;

    QList<QUrl> imAddressList;
    Q_FOREACH (const CDTpContactPtr &contactWrapper, accountWrapper->contacts()) {
        imAddressList << contactImAddress(contactWrapper);
    }

    bool foundOne = false;
    CDTpStorageSyncOperations &op = mSyncOperations[accountWrapper];
    Q_FOREACH (const CDTpContactsSelectItem &item, contactsQuery->items()) {
        if (imAddressList.contains(item.imAddress)) {
            continue;
        }
        foundOne = true;
        op.nContactsRemoved++;
        addRemoveContactToQuery(updateQuery, inserts, deletions, item);
        addRemoveContactFromAccountToQuery(accountDeletions, item);
    }

    if (!foundOne) {
        oneSyncOperationFinished(accountWrapper);
        return;
    }

    updateQuery.addDeletion(deletions, QUrl(defaultGraph));
    updateQuery.addInsertion(inserts, QUrl(defaultGraph));
    updateQuery.addDeletion(accountDeletions, QUrl(privateGraph));

    QList<CDTpAccountPtr> accounts = QList<CDTpAccountPtr>() << accountWrapper;
    CDTpAccountsUpdateQuery *q = new CDTpAccountsUpdateQuery(accounts, updateQuery, this);
    connect(q,
            SIGNAL(finished(CDTpUpdateQuery *)),
            SLOT(onAccountsUpdateQueryFinished(CDTpUpdateQuery *)));
}

void CDTpStorage::onAccountDeleteSelectQueryFinished(CDTpSelectQuery *query)
{
    RDFUpdate updateQuery;
    RDFStatementList deletions;
    RDFStatementList inserts;
    CDTpContactsSelectQuery *contactsQuery = qobject_cast<CDTpContactsSelectQuery*>(query);
    Q_FOREACH (const CDTpContactsSelectItem &item, contactsQuery->items()) {
        addRemoveContactToQuery(updateQuery, inserts, deletions, item);
    }

    updateQuery.addDeletion(deletions, QUrl(defaultGraph));
    updateQuery.addInsertion(inserts, QUrl(defaultGraph));
    new CDTpUpdateQuery(updateQuery, this);
}

void CDTpStorage::onContactDeleteSelectQueryFinished(CDTpSelectQuery *query)
{
    RDFUpdate updateQuery;
    RDFStatementList deletions;
    RDFStatementList accountDeletions;
    RDFStatementList inserts;

    CDTpContactsSelectQuery *contactsQuery = qobject_cast<CDTpContactsSelectQuery*>(query);
    Q_FOREACH (const CDTpContactsSelectItem &item, contactsQuery->items()) {
        addRemoveContactToQuery(updateQuery, inserts, deletions, item);
        addRemoveContactFromAccountToQuery(accountDeletions, item);
    }

    updateQuery.addDeletion(deletions, QUrl(defaultGraph));
    updateQuery.addInsertion(inserts, QUrl(defaultGraph));
    updateQuery.addDeletion(accountDeletions, QUrl(privateGraph));
    new CDTpUpdateQuery(updateQuery, this);
}

void CDTpStorage::addRemoveContactInfoToQuery(RDFUpdate &query,
        const RDFVariable &imContact,
        const QUrl &graph)
{
    query.addDeletion(RDFVariable(), rdf::type::iri(), rdfs::Resource::iri(), graph);
    query.addDeletion(imContact, nco::hasAffiliation::iri(), RDFVariable(), graph);
    query.addDeletion(imContact, nco::birthDate::iri(), RDFVariable(), graph);
    query.addDeletion(imContact, nco::note::iri(), RDFVariable(), graph);
}

QUrl CDTpStorage::contactImAddress(const QString &contactAccountObjectPath,
        const QString &contactId)
{
    return QUrl(QString("telepathy:%1!%2")
            .arg(contactAccountObjectPath)
            .arg(contactId));
}

QUrl CDTpStorage::contactImAddress(CDTpContactPtr contactWrapper)
{
    CDTpAccountPtr accountWrapper = contactWrapper->accountWrapper();
    Tp::AccountPtr account = accountWrapper->account();
    Tp::ContactPtr contact = contactWrapper->contact();
    return contactImAddress(account->objectPath(), contact->id());
}

QUrl CDTpStorage::contactAffiliation(const QString &contactAccountObjectPath,
        const QString &contactId)
{
    return QUrl(QString("affiliationtelepathy:%1!%2")
            .arg(contactAccountObjectPath)
            .arg(contactId));
}

QUrl CDTpStorage::contactAffiliation(CDTpContactPtr contactWrapper)
{
    CDTpAccountPtr accountWrapper = contactWrapper->accountWrapper();
    Tp::AccountPtr account = accountWrapper->account();
    Tp::ContactPtr contact = contactWrapper->contact();
    return contactAffiliation(account->objectPath(), contact->id());
}

QUrl CDTpStorage::trackerStatusFromTpPresenceStatus(
        const QString &tpPresenceStatus)
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

void CDTpStorage::onAccountsUpdateQueryFinished(CDTpUpdateQuery *query)
{
    CDTpAccountsUpdateQuery *accountsQuery = qobject_cast<CDTpAccountsUpdateQuery *>(query);

    Q_FOREACH (const CDTpAccountPtr &accountWrapper, accountsQuery->accounts()) {
        oneSyncOperationFinished(accountWrapper);
    }
}

/* --- CDTpStorageBuilder --- */

CDTpStorageBuilder::CDTpStorageBuilder() : vCount(0)
{
}

void CDTpStorageBuilder::createResource(const QString &resource, const QString &type, const QString &graph)
{
    insertPart[graph] << QString(QLatin1String("%1 a %2")).arg(resource).arg(type);
}

void CDTpStorageBuilder::insertProperty(const QString &resource, const QString &property, const QString &value, const QString &graph)
{
    insertPart[graph] << QString(QLatin1String("%1 %2 %3")).arg(resource).arg(property).arg(value);
}

void CDTpStorageBuilder::deleteResource(const QString &resource)
{
    deletePart << QString(QLatin1String("%1 a rdfs:Resource")).arg(resource);
}

QString CDTpStorageBuilder::deleteProperty(const QString &resource, const QString &property)
{
    const QString value = uniquify();

    deletePart << QString(QLatin1String("%1 %2 %3")).arg(resource).arg(property).arg(value);
    deletePartWhere << QString(QLatin1String("OPTIONAL { %1 %2 %3 }"))
            .arg(resource).arg(property).arg(value);

    return value;
}

QString CDTpStorageBuilder::deleteProperty(const QString &resource, const QString &property, const QString &graph)
{
    const QString value = uniquify();

    deletePart << QString(QLatin1String("%1 %2 %3")).arg(resource).arg(property).arg(value);
    deletePartWhere << QString(QLatin1String("OPTIONAL { GRAPH %1 { %2 %3 %4 } }"))
            .arg(graph).arg(resource).arg(property).arg(value);

    return value;
}


QString CDTpStorageBuilder::deletePropertyAndLinkedResource(const QString &resource, const QString &property)
{
    const QString oldValue = deleteProperty(resource, property);
    deleteResource(oldValue);
    return oldValue;
}

QString CDTpStorageBuilder::updateProperty(const QString &resource, const QString &property, const QString &value, const QString &graph)
{
    const QString oldValue = deleteProperty(resource, property);
    insertProperty(resource, property, value, graph);
    return oldValue;
}

void CDTpStorageBuilder::addCustomSelection(const QString &str)
{
    customSelection << str;
}

QString CDTpStorageBuilder::uniquify(const QString &v)
{
    return QString(QLatin1String("%1_%2")).arg(v).arg(++vCount);
}

QString CDTpStorageBuilder::getRawQuery() const
{
    static QString indent = QLatin1String("    ");
    static QString indent2 = indent + indent;

    // DELETE part
    QString deleteLines = join(deletePart, indent);
    QString deleteWhereLines = join(deletePartWhere, indent);

    // INSERT part
    QString insertLines;
    QString insertWhereLines;
    QHash<QString, QStringList>::const_iterator i;
    for (i = insertPart.constBegin(); i != insertPart.constEnd(); ++i) {
        QString graphLines = join(i.value(), indent2);
        if (!graphLines.isEmpty()) {
            insertLines += indent + QString(QLatin1String("GRAPH %1 {\n")).arg(i.key());
            insertLines += graphLines;
            insertLines += indent + QLatin1String("}\n");
        }
    }

    // Custom selection
    QString customSelectionLines = join(customSelection, indent);
    insertWhereLines += customSelectionLines;
    deleteWhereLines += customSelectionLines;

    // Build final query
    QString rawQuery;
    if (!deleteLines.isEmpty()) {
        rawQuery += QString(QLatin1String("DELETE {\n%1}\n")).arg(deleteLines);
        if (!deleteWhereLines.isEmpty()) {
            rawQuery += QString(QLatin1String("WHERE {\n%1}\n")).arg(deleteWhereLines);
        }
    }
    if (!insertLines.isEmpty()) {
        rawQuery += QString(QLatin1String("INSERT {\n%1}\n")).arg(insertLines);
        if (!insertWhereLines.isEmpty()) {
            rawQuery += QString(QLatin1String("WHERE {\n%1}\n")).arg(insertWhereLines);
        }
    }

    return rawQuery;
}

QString CDTpStorageBuilder::join(const QStringList &lines, const QString &indent) const
{
    QString result;

    Q_FOREACH (const QString &line, lines) {
        result += indent + line + QLatin1String(".\n");
    }

    return result;
}

QSparqlQuery CDTpStorageBuilder::getSparqlQuery() const
{
    return QSparqlQuery(getRawQuery(), QSparqlQuery::InsertStatement);
}

void CDTpStorageBuilder::reset()
{
    deletePart.clear();
    insertPart.clear();
    customSelection.clear();
    vCount = 0;
}

/* --- CDTpStorageSyncOperations --- */

CDTpStorageSyncOperations::CDTpStorageSyncOperations() : active(false),
    nPendingOperations(0), nContactsAdded(0), nContactsRemoved(0)
{
}

