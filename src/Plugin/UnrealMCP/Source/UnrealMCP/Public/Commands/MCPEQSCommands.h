#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * MCP command handler for EQS (Environment Query System) operations.
 * 8 tools: create/read queries, add/remove options and tests, set properties, list types.
 */
class UNREALMCP_API FMCPEQSCommands
{
public:
	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	TSharedPtr<FJsonObject> HandleCreateEQSQuery(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleReadEQSQuery(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleAddEQSOption(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleAddEQSTest(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleRemoveEQSOption(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleRemoveEQSTest(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleSetEQSProperty(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleListEQSTypes(const TSharedPtr<FJsonObject>& Params);
};
