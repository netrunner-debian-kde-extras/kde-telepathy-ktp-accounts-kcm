/*
 * This file is part of telepathy-accounts-kcm
 *
 * Copyright (C) 2009 Collabora Ltd. <info@collabora.com>
 * Copyright (C) 2011 Dominik Schmidt <kde@dominik-schmidt.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "edit-account-dialog.h"

#include <KTp/wallet-interface.h>
#include <KTp/pending-wallet.h>
#include <KTp/wallet-utils.h>

#include "KCMTelepathyAccounts/dictionary.h"
#include "KCMTelepathyAccounts/abstract-account-parameters-widget.h"
#include "KCMTelepathyAccounts/abstract-account-ui.h"
#include "KCMTelepathyAccounts/account-edit-widget.h"
#include "KCMTelepathyAccounts/parameter-edit-widget.h"
#include "KCMTelepathyAccounts/parameter-edit-model.h"
#include "KCMTelepathyAccounts/plugin-manager.h"

#include <KDebug>
#include <KLocale>
#include <KTabWidget>

#include <QtCore/QList>

#include <TelepathyQt/ConnectionManager>
#include <TelepathyQt/PendingStringList>

class EditAccountDialog::Private
{
public:
    Private()
            : item(0), widget(0), reconnectRequired(false), kwalletReady(false)
    {
    }

    AccountItem *item;
    AccountEditWidget *widget;
    bool reconnectRequired;
    bool kwalletReady;
};

EditAccountDialog::EditAccountDialog(AccountItem *item, QWidget *parent)
        : KDialog(parent),
          d(new Private)
{
    d->item = item;

    connect(KTp::WalletInterface::openWallet(), SIGNAL(finished(Tp::PendingOperation*)), SLOT(onWalletOpened(Tp::PendingOperation*)));

    setMinimumWidth(400);
}

EditAccountDialog::~EditAccountDialog()
{
    delete d;
}

void EditAccountDialog::onWalletOpened(Tp::PendingOperation *op)
{
    KTp::PendingWallet *walletOp = qobject_cast<KTp::PendingWallet*>(op);
    Q_ASSERT(walletOp);

    KTp::WalletInterface *walletInterface = walletOp->walletInterface();

    // Get the protocol's parameters and values.
    Tp::ProtocolInfo protocolInfo = d->item->account()->protocolInfo();
    Tp::ProtocolParameterList parameters = protocolInfo.parameters();
    QVariantMap parameterValues = d->item->account()->parameters();

    // Add the parameters to the model.
    ParameterEditModel *parameterModel = new ParameterEditModel(this);
    parameterModel->addItems(parameters, d->item->account()->profile()->parameters(), parameterValues);

    //update the parameter model with the password from kwallet (if applicable)
    Tp::ProtocolParameter passwordParameter = parameterModel->parameter(QLatin1String("password"));

    if (passwordParameter.isValid() && walletInterface->hasPassword(d->item->account())) {
        QModelIndex index = parameterModel->indexForParameter(passwordParameter);
        QString password = walletInterface->password(d->item->account());
        parameterModel->setData(index, password, Qt::EditRole);
    }


    // Set up the interface
    d->widget = new AccountEditWidget(d->item->account()->profile(),
                                      parameterModel,
                                      doNotConnectOnAdd,
                                      this);
    setMainWidget(d->widget);

    d->kwalletReady = true;
    show();
}

void EditAccountDialog::accept()
{
    if (!d->widget) {
        kWarning() << "missing d->widget, not saving parameters";
        return;
    }

    QVariantMap setParameters = d->widget->parametersSet();
    QStringList unsetParameters = d->widget->parametersUnset();

    // Check all pages of parameters pass validation.
    if (!d->widget->validateParameterValues()) {
        kDebug() << "A widget failed parameter validation. Not accepting wizard.";
        return;
    }

    //remove password from setParameters as this is now stored by kwallet instead
    setParameters.remove(QLatin1String("password"));

    Tp::PendingStringList *psl = d->item->account()->updateParameters(setParameters, unsetParameters);

    kDebug() << "Set parameters:" << setParameters;
    kDebug() << "Unset parameters:" << unsetParameters;

    connect(psl,
            SIGNAL(finished(Tp::PendingOperation*)),
            SLOT(onParametersUpdated(Tp::PendingOperation*)));
}

void EditAccountDialog::onParametersUpdated(Tp::PendingOperation *op)
{
    if (op->isError()) {
        // FIXME: Visual feedback in GUI to user.
        kWarning() << "Could not update parameters:" << op->errorName() << op->errorMessage();
        return;
    }

    Tp::PendingStringList *psl = qobject_cast<Tp::PendingStringList*>(op);

    Q_ASSERT(psl);
    if (!psl) {
        kWarning() << "Something  weird happened";
    }

    if (psl->result().size() > 0) {
        kDebug() << "The following parameters won't be updated until reconnection: " << psl->result();
        d->reconnectRequired = true;
    }

    QVariantMap values = d->widget->parametersSet();

    if (values.contains(QLatin1String("password"))) {
        KTp::WalletUtils::setAccountPassword(d->item->account(), values[QLatin1String("password")].toString());
    } else {
        KTp::WalletUtils::setAccountPassword(d->item->account(), QString());
    }


    // FIXME: Ask the user to submit a Display Name
    QString displayName;
    if (values.contains(QLatin1String("account"))) {
        displayName = values[QLatin1String("account")].toString();
    }
    else {
        displayName = d->item->account()->profile()->protocolName();
    }

    Tp::PendingOperation *dnop = d->item->account()->setDisplayName(displayName);

    connect(dnop,
            SIGNAL(finished(Tp::PendingOperation*)),
            SLOT(onDisplayNameUpdated(Tp::PendingOperation*)));
}

void EditAccountDialog::onDisplayNameUpdated(Tp::PendingOperation *op)
{
    if (op->isError()) {
        // FIXME: Visual feedback in GUI to user.
        kWarning() << "Could not update display name:" << op->errorName() << op->errorMessage();
        return;
    }

    Q_EMIT finished();

    if (d->reconnectRequired) {
        d->item->account()->reconnect();
    }

    // set the dialog as accepted and exit
    done(KDialog::Accepted);
}

void EditAccountDialog::setVisible(bool visible)
{
    if (visible && d->kwalletReady) {
	KDialog::setVisible(visible);
	return;
    }

    KDialog::setVisible(false);
}

#include "edit-account-dialog.moc"
