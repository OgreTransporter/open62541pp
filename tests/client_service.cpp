#include <doctest/doctest.h>

#include "open62541pp/client.hpp"
#include "open62541pp/detail/open62541/common.h"
#include "open62541pp/server.hpp"
#include "open62541pp/services/detail/client_service.hpp"
#include "open62541pp/types_composed.hpp"  // ReadResponse

#include "helper/server_runner.hpp"

using namespace opcua;

TEST_CASE("AsyncServiceAdapter") {
    SUBCASE("createCallbackAndContext") {
        using Response = int;
        using Adapter = services::detail::AsyncServiceAdapter<Response>;

        std::optional<Response> result;
        bool throwInCompletionHandler = false;
        detail::ExceptionCatcher catcher;

        auto callbackAndContext = Adapter::createCallbackAndContext(
            catcher,
            [&](Response res) {
                result = res;
                if (throwInCompletionHandler) {
                    throw std::runtime_error("CompletionHandler");
                }
            }
        );

        const auto invokeCallback = [&](int* response) {
            callbackAndContext.callback(
                nullptr,  // client
                callbackAndContext.context.release(),
                0,  // requestId
                response
            );
        };

        Response response = 5;
        SUBCASE("Success") {
            invokeCallback(&response);
            CHECK(result.has_value());
            CHECK(result.value() == response);
            CHECK_FALSE(catcher.hasException());
        }
        SUBCASE("Response nullptr") {
            invokeCallback(nullptr);
            CHECK_FALSE(result.has_value());
            CHECK(catcher.hasException());
            CHECK_THROWS_AS_MESSAGE(catcher.rethrow(), BadStatus, "BadUnexpectedError");
        }
        SUBCASE("Exception in completion handler") {
            throwInCompletionHandler = true;
            invokeCallback(&response);
            CHECK(result.has_value());
            CHECK(result.value() == response);
            CHECK(catcher.hasException());
            CHECK_THROWS_AS_MESSAGE(catcher.rethrow(), std::runtime_error, "CompletionHandler");
        }
    }
}

TEST_CASE("sendRequest") {
    Client client;

    auto sendReadRequest = [&] {
        UA_ReadValueId item{};
        item.nodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
        item.attributeId = UA_ATTRIBUTEID_BROWSENAME;
        UA_ReadRequest request{};
        request.timestampsToReturn = UA_TIMESTAMPSTORETURN_BOTH;
        request.nodesToReadSize = 1;
        request.nodesToRead = &item;
        return services::detail::sendRequest<UA_ReadRequest, ReadResponse>(client, request);
    };

    SUBCASE("Disconnected") {
        const auto response = sendReadRequest();
        // UA_STATUSCODE_BADCONNECTIONCLOSED or UA_STATUSCODE_BADINTERNALERROR (v1.0)
        CHECK(response.getResponseHeader().getServiceResult().isBad());
    }

    Server server;
    ServerRunner serverRunner(server);
    client.connect("opc.tcp://localhost:4840");

    SUBCASE("Success") {
        const auto response = sendReadRequest();
        CHECK(response.getResponseHeader().getServiceResult().isGood());
        CHECK_EQ(
            response.getResults()[0].getValue().getScalar<QualifiedName>(),
            QualifiedName(0, "Objects")
        );
    }
}

TEST_CASE("sendRequestAsync") {
    Client client;

    auto sendReadRequest = [&](auto&& token) {
        UA_ReadValueId item{};
        item.nodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
        item.attributeId = UA_ATTRIBUTEID_BROWSENAME;
        UA_ReadRequest request{};
        request.timestampsToReturn = UA_TIMESTAMPSTORETURN_BOTH;
        request.nodesToReadSize = 1;
        request.nodesToRead = &item;
        return services::detail::sendRequestAsync<UA_ReadRequest, ReadResponse>(
            client,
            request,
            std::forward<decltype(token)>(token)
        );
    };

    SUBCASE("Disconnected") {
        sendReadRequest([&](ReadResponse& response) {
            // UA_STATUSCODE_BADSERVERNOTCONNECTED since v1.1
            CHECK(response.getResponseHeader().getServiceResult().isBad());
        });
    }

    Server server;
    ServerRunner serverRunner(server);
    client.connect("opc.tcp://localhost:4840");

    SUBCASE("Success") {
        bool executed = false;
        sendReadRequest([&](ReadResponse& response) {
            CHECK(response.getResponseHeader().getServiceResult().isGood());
            CHECK_EQ(
                response.getResults()[0].getValue().getScalar<QualifiedName>(),
                QualifiedName(0, "Objects")
            );
            executed = true;
        });
        CHECK_NOTHROW(client.runIterate());
        CHECK(executed);
    }

    SUBCASE("Exception in user callback") {
        sendReadRequest([](ReadResponse&) { throw std::runtime_error("Error"); });
        CHECK_THROWS_AS_MESSAGE(client.runIterate(), std::runtime_error, "Error");
    }
}