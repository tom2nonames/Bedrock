// /srs/bedrock/BedrockNode.cpp
#include <libstuff/libstuff.h>
#include <libstuff/version.h>
#include "BedrockNode.h"
#include "BedrockPlugin.h"
#include "BedrockServer.h"
#include <iomanip>

// *jsonCode* Values
// -----------------------
// For consistency, all API commands return response codes in the following categories:
//
// ### 2xx Class ###
// Any response between 200 and 299 means the request was valid and accepted.
//
// * 200 OK
//
// ### 3xx Class ###
// Any response between 300 and 399 means that the request was valid, but rejected
// for some reason less than failure.
//
// * 300 Redundant request
// * 301 Limit hit.
// * 302 Invalid validateCode (for bank account validation)
//
// ### 4xx Class ###
// Any response between 400 and 499 means the request was valid, but failed.
//
// * 400 Unknown request failure
// * 401 Unauthorized
// * 402 Incomplete request
// * 403 Terrorist <-- no longer used, but left in for nostalgia.
// * 404 Resource doesn't exist
// * 405 Resource in incorrect state
// * 410 Resource not ready.
// * 411 Insufficient privileges
// * 412 Down for maintenance (used in waf)
//
// ### 5xx Class ###
// Any response between 500 and 599 indicates the server experienced some internal
// failure, and it's unknown if the request was valid.
//
// * 500 Unknown server failure
// * 501 Transaction failure
// * 502 Failed to execute query
// * 503 Query returned invalid response
// * 504 Resource in invalid state
// * 507 Vendor error
// * 508 Live operation not enabled
// * 509 Operation timed out.
// * 530 Unexpected response.
// * 531 Expected but unusable response, retry later.
// * 534 Unexpected HTTP request/response - usually timeout or 500 level server error.

BedrockNode::BedrockNode(const SData& args, BedrockServer* server_)
    : SQLiteNode(args["-db"], args["-nodeName"], args["-nodeHost"], args.calc("-priority"), args.calc("-cacheSize"),
                 1024,                                                 // auto-checkpoint every 1024 pages
                 STIME_US_PER_M * 2 + SRand64() % STIME_US_PER_S * 30, // Be patient first time
                 server_->getVersion(), args.calc("-quorumCheckpoint"), args["-synchronousCommands"],
                 args.test("-readOnly"), args.calc("-maxJournalSize")),
      server(server_) {
    // Initialize
    SINFO("BedrockNode constructor");
}

BedrockNode::~BedrockNode() {
    // Note any orphaned commands; this list should ideally be empty
    list<string> commandList;
    commandList = getQueuedCommandList();
    if (!commandList.empty())
        SALERT("Queued: " << SComposeJSONArray(commandList));
}

void BedrockNode::postSelect(fd_map& fdm, uint64_t& nextActivity) {
    // Update the parent and attributes
    SQLiteNode::postSelect(fdm, nextActivity);
}

bool BedrockNode::isReadOnly() { return _readOnly; }

bool BedrockNode::_peekCommand(SQLite& db, Command* command) {
    // Classify the message
    SData& request = command->request;
    SData& response = command->response;
    STable& content = command->jsonContent;
    SDEBUG("Peeking at '" << request.methodLine << "'");

    // Assume success; will throw failure if necessary
    response.methodLine = "200 OK";
    try {
        // Loop across the plugins to see which wants to take this
        bool pluginPeeked = false;
        SFOREACH (list<BedrockPlugin*>, *BedrockPlugin::g_registeredPluginList, pluginIt) {
            // See if it peeks this
            BedrockPlugin* plugin = *pluginIt;
            if (plugin->enabled() && plugin->peekCommand(this, db, command)) {
                // Peeked it!
                SINFO("Plugin '" << plugin->getName() << "' peeked command '" << request.methodLine << "'");
                pluginPeeked = true;
                break;
            }
        }

        // If not peeked by a plugin, do the old commands
        if (!pluginPeeked) {
            // Not a peekable command
            SINFO("Command '" << request.methodLine << "' is not peekable, queuing for processing.");
            return false; // Not done
        }

        // Success.  If a command has set "content", encode it in the response.
        SINFO("Responding '" << response.methodLine << "' to read-only '" << request.methodLine << "'.");
        if (!content.empty()) {
            // Make sure we're not overwriting anything different.
            string newContent = SComposeJSONObject(content);
            if (response.content != newContent) {
                if (!response.content.empty()) {
                    SWARN("Replacing existing response content in " << request.methodLine);
                }
                response.content = newContent;
            }
        }
    } catch (const char* e) {
        // Error -- roll back the database and return the error
        const string& msg = "Error processing read-only command '" + request.methodLine + "' (" + e + "), ignoring: " +
                            request.serialize();
        if (SContains(e, "_ALERT_"))
            SALERT(msg);
        else if (SContains(e, "_WARN_"))
            SWARN(msg);
        else if (SContains(e, "_HMMM_"))
            SHMMM(msg);
        else if (SStartsWith(e, "50"))
            SALERT(msg); // Alert on 500 level errors.
        else
            SINFO(msg);
        response.methodLine = e;
    }

    // If we get here, it means the command is fully completed.
    return true;
}

void BedrockNode::_processCommand(SQLite& db, Command* command) {
    // Classify the message
    SData& request = command->request;
    SData& response = command->response;
    STable& content = command->jsonContent;
    SDEBUG("Received '" << request.methodLine << "'");
    try {
        // Process the message
        if (!db.beginTransaction())
            throw "501 Failed to begin transaction";

        // --------------------------------------------------------------------------
        if (SIEquals(request.methodLine, "UpgradeDatabase")) {
            // Loop across the plugins to give each an opportunity to upgrade the
            // database.  This command is triggered only on the MASTER, and only
            // upon it step up in the MASTERING state.
            SINFO("Upgrading database");
            for_each(BedrockPlugin::g_registeredPluginList->begin(), BedrockPlugin::g_registeredPluginList->end(),
                     [this, &db](BedrockPlugin* plugin) {
                         // See if it processes this
                         if (plugin->enabled()) {
                             plugin->upgradeDatabase(this, db);
                         }
                     });
            SINFO("Finished upgrading database");
        } else {
            // --------------------------------------------------------------------------
            // Loop across the plugins to see which wants to take this
            bool pluginProcessed = false;
            SFOREACH (list<BedrockPlugin*>, *BedrockPlugin::g_registeredPluginList, pluginIt) {
                // See if it processes this
                BedrockPlugin* plugin = *pluginIt;
                if (plugin->enabled() && plugin->processCommand(this, db, command)) {
                    // Processed it!
                    SINFO("Plugin '" << plugin->getName() << "' processed command '" << request.methodLine << "'");
                    pluginProcessed = true;
                    break;
                }
            }

            // If no plugin processed it, respond accordingly
            if (!pluginProcessed) {
                // No command specified
                SWARN("Command '" << request.methodLine << "' does not exist.");
                throw "430 Unrecognized command";
            }
        }

        // If we have no uncommitted query, just rollback the empty transaction.
        // Otherwise, try to prepare to commit.
        bool isQueryEmpty = db.getUncommittedQuery().empty();
        if (isQueryEmpty)
            db.rollback();
        else if (!db.prepare())
            throw "501 Failed to prepare transaction";

        // Success, this command will be committed.
        SINFO("Responding '" << response.methodLine << "' to '" << request.methodLine << "'.");

        // Finally, if a command has set "content", encode it in the response.
        if (!content.empty()) {
            // Make sure we're not overwriting anything different.
            string newContent = SComposeJSONObject(content);
            if (response.content != newContent) {
                if (!response.content.empty()) {
                    SWARN("Replacing existing response content in " << request.methodLine);
                }
                response.content = newContent;
            }
        }

    } catch (const char* e) {
        // Error -- roll back the database and return the error
        db.rollback();
        const string& msg =
            "Error processing command '" + request.methodLine + "' (" + e + "), ignoring: " + request.serialize();
        if (SContains(e, "_ALERT_"))
            SALERT(msg);
        else if (SContains(e, "_WARN_"))
            SWARN(msg);
        else if (SContains(e, "_HMMM_"))
            SHMMM(msg);
        else if (SStartsWith(e, "50"))
            SALERT(msg); // Alert on 500 level errors.
        else
            SINFO(msg);
        response.methodLine = e;
    }
}

// Notes that we failed to process something
void BedrockNode::_abortCommand(SQLite& db, Command* command) {
    // Note the failure in the response
    command->response.methodLine = "500 ABORTED";
}

void BedrockNode::_cleanCommand(Command* command) {
    if (command->httpsRequest) {
        if (command->httpsRequest->owner) {
            command->httpsRequest->owner->closeTransaction(command->httpsRequest);
        } else {
            SERROR("No owner for this https request " << command->httpsRequest->fullResponse.methodLine);
        }
        command->httpsRequest = 0;
    }
}