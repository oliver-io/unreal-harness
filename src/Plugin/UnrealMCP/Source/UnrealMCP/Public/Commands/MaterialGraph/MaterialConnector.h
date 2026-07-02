#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Wires material expression outputs to expression inputs or material inputs.
 *
 * Two connection modes:
 *   1. Expression → Expression   (target_expression = expression name/index)
 *   2. Expression → Material     (target_expression = "Material", target_input = "BaseColor" etc.)
 */
class FMaterialConnector
{
public:
	static TSharedPtr<FJsonObject> ConnectExpressions(const TSharedPtr<FJsonObject>& Params);
};
