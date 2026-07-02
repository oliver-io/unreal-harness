#pragma once

#include "CoreMinimal.h"
#include "Json.h"

class UMaterial;
class UMaterialExpression;

/**
 * Manages material expression CRUD: add, delete, set property, read graph.
 */
class FMaterialExpressionManager
{
public:
	static TSharedPtr<FJsonObject> AddExpression(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> DeleteExpression(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> SetExpressionProperty(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> ReadMaterialGraph(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> ReadMaterialFunction(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> ReadMaterialInstance(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> SetMaterialInstanceParameter(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> ReparentMaterialInstance(const TSharedPtr<FJsonObject>& Params);

	/** Find an expression in a material by UObject name or array index string. */
	static UMaterialExpression* FindExpression(UMaterial* Material, const FString& ExpressionId);

private:
	/** Build a JSON description of a single expression. */
	static TSharedPtr<FJsonObject> ExpressionToJson(UMaterialExpression* Expr, int32 Index);
};
