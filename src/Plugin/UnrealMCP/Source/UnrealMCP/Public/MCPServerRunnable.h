#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Sockets.h"
#include "Interfaces/IPv4/IPv4Address.h"

class UMCPBridge;

/**
 * Runnable class for the MCP server thread
 */
class FMCPServerRunnable : public FRunnable
{
public:
	FMCPServerRunnable(UMCPBridge* InBridge, TSharedPtr<FSocket> InListenerSocket);
	virtual ~FMCPServerRunnable();

	// FRunnable interface
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

protected:
	void HandleClientConnection(TSharedPtr<FSocket> ClientSocket);
	void ProcessMessage(TSharedPtr<FSocket> Client, const FString& Message);

private:
	UMCPBridge* Bridge;
	TSharedPtr<FSocket> ListenerSocket;
	TSharedPtr<FSocket> ClientSocket;
	bool bRunning;
}; 