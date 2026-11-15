/*
  Copyright (C) 2026 Kieren (DCC file transfers)

  You may use this file under the terms of BSD license as follows:

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR
  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

import QtQuick 2.1
import Sailfish.Silica 1.0

// Lists DCC file transfers: offers to accept or decline, transfers in
// progress, and finished ones (open the file, or remove from the list).
Page {
    id: page

    allowedOrientations: window.allowedOrientations

    SilicaListView {
        id: view
        anchors.fill: parent
        model: DccManager

        header: PageHeader { title: qsTr("File transfers") }

        PullDownMenu {
            visible: DccManager.finishedTransfers > 0
            MenuItem {
                text: qsTr("Clear finished")
                onClicked: DccManager.clearFinished()
            }
        }

        ViewPlaceholder {
            enabled: view.count === 0
            text: qsTr("No file transfers")
            hintText: qsTr("To send a file, open a private chat and pull down")
        }

        delegate: ListItem {
            id: item
            contentHeight: column.height + 2 * Theme.paddingMedium
            menu: contextMenu

            function openFile() {
                Qt.openUrlExternally(DccManager.fileUrl(transferId))
            }

            onClicked: {
                if (model.done && model.incoming)
                    openFile()
                else
                    openMenu()
            }

            Column {
                id: column
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.paddingSmall

                Label {
                    width: parent.width
                    text: model.fileName
                    truncationMode: TruncationMode.Fade
                    color: item.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
                Label {
                    width: parent.width
                    text: (model.incoming ? qsTr("From %1").arg(model.nick) : qsTr("To %1").arg(model.nick))
                          + (model.network ? " · " + model.network : "")
                          + (model.sizeText ? " · " + model.sizeText : "")
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: Theme.secondaryColor
                    truncationMode: TruncationMode.Fade
                }
                ProgressBar {
                    visible: model.active
                    width: parent.width + 2 * Theme.horizontalPageMargin
                    x: -Theme.horizontalPageMargin
                    minimumValue: 0
                    maximumValue: 1
                    value: model.progress
                    label: model.statusText
                }
                Label {
                    visible: !model.active
                    width: parent.width
                    text: model.statusText
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: model.status === 5 ? window.nickHighlight // failed
                                              : (model.offered ? Theme.highlightColor : Theme.secondaryColor)
                }
                Row {
                    visible: model.offered
                    spacing: Theme.paddingMedium
                    Button {
                        text: qsTr("Accept")
                        onClicked: DccManager.accept(transferId)
                    }
                    Button {
                        text: qsTr("Decline")
                        onClicked: DccManager.decline(transferId)
                    }
                }
            }

            Component {
                id: contextMenu
                ContextMenu {
                    MenuItem {
                        text: qsTr("Accept")
                        visible: model.offered
                        onClicked: DccManager.accept(transferId)
                    }
                    MenuItem {
                        text: qsTr("Decline")
                        visible: model.offered
                        onClicked: DccManager.decline(transferId)
                    }
                    MenuItem {
                        text: qsTr("Open")
                        visible: model.done && model.incoming
                        onClicked: item.openFile()
                    }
                    MenuItem {
                        text: qsTr("Cancel")
                        visible: model.active
                        onClicked: DccManager.cancel(transferId)
                    }
                    MenuItem {
                        text: qsTr("Remove from list")
                        visible: model.finished
                        onClicked: DccManager.remove(transferId)
                    }
                }
            }
        }

        VerticalScrollDecorator { }
    }
}
