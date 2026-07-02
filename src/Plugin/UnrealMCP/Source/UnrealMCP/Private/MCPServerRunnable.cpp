#include "MCPServerRunnable.h"
#include "Commands/MCPCommonUtils.h"
#include "MCPBridge.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "JsonObjectConverter.h"
#include "Misc/ScopeLock.h"
#include "HAL/PlatformTime.h"

FMCPServerRunnable::FMCPServerRunnable(UMCPBridge* InBridge, TSharedPtr<FSocket> InListenerSocket)
    : Bridge(InBridge)
    , ListenerSocket(InListenerSocket)
    , bRunning(true)
{
    UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Created server runnable"));
}

FMCPServerRunnable::~FMCPServerRunnable()
{
    // Note: We don't delete the sockets here as they're owned by the bridge
}

bool FMCPServerRunnable::Init()
{
    return true;
}

uint32 FMCPServerRunnable::Run()
{
    UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Server thread starting..."));
    
    while (bRunning)
    {
        bool bPending = false;
        if (ListenerSocket->HasPendingConnection(bPending) && bPending)
        {
            UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Client connection pending, accepting..."));
            
            ClientSocket = MakeShareable(ListenerSocket->Accept(TEXT("MCPClient")));
            if (ClientSocket.IsValid())
            {
                UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Client connection accepted"));
                
                // Set socket options to improve connection stability
                ClientSocket->SetNoDelay(true);
                int32 SocketBufferSize = 65536;  // 64KB buffer
                ClientSocket->SetSendBufferSize(SocketBufferSize, SocketBufferSize);
                ClientSocket->SetReceiveBufferSize(SocketBufferSize, SocketBufferSize);
                
                uint8 Buffer[8192];
                // Accumulate bytes across Recv calls until the buffer forms a complete JSON object. The client
                // framing (server/src/bridge/connection.ts) is connection-per-command, NO delimiter, "read until
                // parseable"; a single Recv is NOT guaranteed to hold the whole command (TCP can split it, and a
                // large command such as a big material Custom-node HLSL body exceeds one 8KB buffer). The old code
                // parsed each Recv in isolation and silently dropped any split/oversized command as "non-JSON",
                // never replying -> the client saw the socket close. Now we read until the buffer parses.
                TArray<uint8> MessageBytes;
                while (bRunning)
                {
                    int32 BytesRead = 0;
                    if (ClientSocket->Recv(Buffer, sizeof(Buffer) - 1, BytesRead))
                    {
                        if (BytesRead == 0)
                        {
                            UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Client disconnected (zero bytes)"));
                            break;
                        }

                        // Append this chunk and try to parse the FULL accumulated buffer.
                        MessageBytes.Append(Buffer, BytesRead);
                        TArray<uint8> NullTerminated = MessageBytes;
                        NullTerminated.Add(0);
                        FString ReceivedText = UTF8_TO_TCHAR(NullTerminated.GetData());

                        // Parse JSON (a partial command will simply fail to deserialize -> keep reading)
                        TSharedPtr<FJsonObject> JsonObject;
                        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ReceivedText);

                        if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
                        {
                            // Complete command received — consume the accumulator.
                            MessageBytes.Reset();
                            UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Received: %s"), *ReceivedText);

                            // Get command type
                            FString CommandType;
                            if (JsonObject->TryGetStringField(TEXT("type"), CommandType))
                            {
                                UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Executing command: %s"), *CommandType);

                                // Execute command
                                FString Response = Bridge->ExecuteCommand(CommandType, JsonObject->GetObjectField(TEXT("params")));

                                UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Command executed, response length: %d"), Response.Len());

                                // Log response for debugging (truncated for large responses)
                                FString LogResponse = Response.Len() > 200 ? Response.Left(200) + TEXT("...") : Response;
                                UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Sending response (%d bytes): %s"),
                                       Response.Len(), *LogResponse);

                                // Convert to UTF8 once
                                FTCHARToUTF8 UTF8Response(*Response);
                                const uint8* DataToSend = (const uint8*)UTF8Response.Get();
                                int32 TotalDataSize = UTF8Response.Length();
                                int32 TotalBytesSent = 0;
                                bool bSuccess = true;

                                // Send all data in a loop (TCP may not send everything at once)
                                while (TotalBytesSent < TotalDataSize)
                                {
                                    int32 BytesSent = 0;
                                    bool bSendResult = ClientSocket->Send(DataToSend + TotalBytesSent,
                                                                          TotalDataSize - TotalBytesSent,
                                                                          BytesSent);

                                    if (!bSendResult)
                                    {
                                        int32 LastError = (int32)ISocketSubsystem::Get()->GetLastErrorCode();
                                        UE_LOG(LogUnrealMCP, Error, TEXT("MCPServerRunnable: Failed to send response after %d/%d bytes - Error code: %d"),
                                               TotalBytesSent, TotalDataSize, LastError);
                                        bSuccess = false;
                                        break;
                                    }

                                    TotalBytesSent += BytesSent;
                                    UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Sent %d bytes (%d/%d total)"),
                                           BytesSent, TotalBytesSent, TotalDataSize);
                                }

                                if (bSuccess)
                                {
                                    UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Response sent successfully (%d bytes)"),
                                           TotalBytesSent);
                                }
                            }
                            else
                            {
                                UE_LOG(LogUnrealMCP, Warning, TEXT("MCPServerRunnable: Missing 'type' field in command"));
                            }
                        }
                        else
                        {
                            // Not a complete JSON object yet. If the buffer doesn't even START with '{' it's a port
                            // PROBE (e.g. an HTTP readiness GET `curl http://127.0.0.1:55557/` -> `GET / HTTP/1.1`),
                            // not an MCP command — drop it. Otherwise it's a partial command split across TCP
                            // segments: keep the bytes and read more (the next Recv appends and we re-parse). A hard
                            // cap bounds a garbage / never-terminating stream.
                            int32 Ws = 0;
                            while (Ws < MessageBytes.Num() && (MessageBytes[Ws] == ' ' || MessageBytes[Ws] == '\t' || MessageBytes[Ws] == '\r' || MessageBytes[Ws] == '\n'))
                            {
                                ++Ws;
                            }
                            const bool bLooksLikeJson = (Ws < MessageBytes.Num()) && (MessageBytes[Ws] == '{');
                            if (!bLooksLikeJson || MessageBytes.Num() > 16 * 1024 * 1024)
                            {
                                UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Discarding %d bytes (non-JSON probe or oversized)"), MessageBytes.Num());
                                MessageBytes.Reset();
                            }
                        }
                    }
                    else
                    {
                        int32 LastError = (int32)ISocketSubsystem::Get()->GetLastErrorCode();
                        // Don't break the connection for WouldBlock error, which is normal for non-blocking sockets
                        bool bShouldBreak = true;
                        
                        // Check for "would block" error which isn't a real error for non-blocking sockets
                        if (LastError == SE_EWOULDBLOCK) 
                        {
                            UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Socket would block, continuing..."));
                            bShouldBreak = false;
                            // Small sleep to prevent tight loop when no data
                            FPlatformProcess::Sleep(0.01f);
                        }
                        // Check for other transient errors we might want to tolerate
                        else if (LastError == SE_EINTR) // Interrupted system call
                        {
                            UE_LOG(LogUnrealMCP, Warning, TEXT("MCPServerRunnable: Socket read interrupted, continuing..."));
                            bShouldBreak = false;
                        }
                        else
                        {
                            UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Client disconnected or error. Last error code: %d"), LastError);
                        }
                        
                        if (bShouldBreak)
                        {
                            break;
                        }
                    }
                }
            }
            else
            {
                UE_LOG(LogUnrealMCP, Warning, TEXT("MCPServerRunnable: Failed to accept client connection"));
            }
        }
        
        // Small sleep to prevent tight loop
        FPlatformProcess::Sleep(0.1f);
    }
    
    UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Server thread stopping"));
    return 0;
}

void FMCPServerRunnable::Stop()
{
    bRunning = false;
}

void FMCPServerRunnable::Exit()
{
}

void FMCPServerRunnable::HandleClientConnection(TSharedPtr<FSocket> InClientSocket)
{
    if (!InClientSocket.IsValid())
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("MCPServerRunnable: Invalid client socket passed to HandleClientConnection"));
        return;
    }

    UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Starting to handle client connection"));
    
    // Set socket options for better connection stability
    InClientSocket->SetNonBlocking(false);
    UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Set socket to blocking mode"));
    
    // Properly read full message with timeout
    const int32 MaxBufferSize = 4096;
    uint8 Buffer[MaxBufferSize];
    FString MessageBuffer;
    
    UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Starting message receive loop"));
    
    while (bRunning && InClientSocket.IsValid())
    {
        // Log socket state
        bool bIsConnected = InClientSocket->GetConnectionState() == SCS_Connected;
        UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Socket state - Connected: %s"), 
               bIsConnected ? TEXT("true") : TEXT("false"));
        
        // Log pending data status before receive
        uint32 PendingDataSize = 0;
        bool HasPendingData = InClientSocket->HasPendingData(PendingDataSize);
        UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Before Recv - HasPendingData=%s, Size=%d"), 
               HasPendingData ? TEXT("true") : TEXT("false"), PendingDataSize);
        
        // Try to receive data with timeout
        int32 BytesRead = 0;
        bool bReadSuccess = false;
        
        UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Attempting to receive data..."));
        bReadSuccess = InClientSocket->Recv(Buffer, MaxBufferSize - 1, BytesRead, ESocketReceiveFlags::None);
        
        UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Recv attempt complete - Success=%s, BytesRead=%d"), 
               bReadSuccess ? TEXT("true") : TEXT("false"), BytesRead);
        
        if (BytesRead > 0)
        {
            // Log raw data for debugging
            FString HexData;
            for (int32 i = 0; i < FMath::Min(BytesRead, 50); ++i)
            {
                HexData += FString::Printf(TEXT("%02X "), Buffer[i]);
            }
            UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Raw data (first 50 bytes hex): %s%s"), 
                   *HexData, BytesRead > 50 ? TEXT("...") : TEXT(""));
            
            // Convert and log received data
            Buffer[BytesRead] = 0; // Null terminate
            FString ReceivedData = UTF8_TO_TCHAR(Buffer);
            UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Received data as string: '%s'"), *ReceivedData);
            
            // Append to message buffer
            MessageBuffer.Append(ReceivedData);
            
            // Process complete messages (messages are terminated with newline)
            if (MessageBuffer.Contains(TEXT("\n")))
            {
                UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Newline detected in buffer, processing messages"));
                
                TArray<FString> Messages;
                MessageBuffer.ParseIntoArray(Messages, TEXT("\n"), true);
                
                UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Found %d message(s) in buffer"), Messages.Num());
                
                // Process all complete messages
                for (int32 i = 0; i < Messages.Num() - 1; ++i)
                {
                    UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Processing message %d: '%s'"), 
                           i + 1, *Messages[i]);
                    ProcessMessage(InClientSocket, Messages[i]);
                }
                
                // Keep any incomplete message in the buffer
                MessageBuffer = Messages.Num() > 0 ? Messages.Last() : FString();
                UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Remaining buffer after processing: %s"), 
                       *MessageBuffer);
            }
            else
            {
                UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: No complete message yet (no newline detected)"));
            }
        }
        else if (!bReadSuccess)
        {
            UE_LOG(LogUnrealMCP, Warning, TEXT("MCPServerRunnable: Connection closed or error occurred - Last error: %d"), 
                   (int32)ISocketSubsystem::Get()->GetLastErrorCode());
            break;
        }
        
        // Small sleep to prevent tight loop
        FPlatformProcess::Sleep(0.01f);
    }
    
    UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Exited message receive loop"));
}

void FMCPServerRunnable::ProcessMessage(TSharedPtr<FSocket> Client, const FString& Message)
{
    UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Processing message: %s"), *Message);
    
    // Parse message as JSON
    TSharedPtr<FJsonObject> JsonMessage;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
    
    if (!FJsonSerializer::Deserialize(Reader, JsonMessage) || !JsonMessage.IsValid())
    {
        UE_LOG(LogUnrealMCP, Warning, TEXT("MCPServerRunnable: Failed to parse message as JSON"));
        return;
    }
    
    // Extract command type and parameters using MCP protocol format
    FString CommandType;
    TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject());
    
    if (!JsonMessage->TryGetStringField(TEXT("command"), CommandType))
    {
        UE_LOG(LogUnrealMCP, Warning, TEXT("MCPServerRunnable: Message missing 'command' field"));
        return;
    }
    
    // Parameters are optional in MCP protocol
    if (JsonMessage->HasField(TEXT("params")))
    {
        TSharedPtr<FJsonValue> ParamsValue = JsonMessage->TryGetField(TEXT("params"));
        if (ParamsValue.IsValid() && ParamsValue->Type == EJson::Object)
        {
            Params = ParamsValue->AsObject();
        }
    }
    
    UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Executing command: %s"), *CommandType);
    
    // Execute command
    FString Response = Bridge->ExecuteCommand(CommandType, Params);
    
    // Send response with newline terminator
    Response += TEXT("\n");

    UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Sending response (%d bytes): %s"),
           Response.Len(), *Response);

    // Convert to UTF8 once
    FTCHARToUTF8 UTF8Response(*Response);
    const uint8* DataToSend = (const uint8*)UTF8Response.Get();
    int32 TotalDataSize = UTF8Response.Length();
    int32 TotalBytesSent = 0;

    // Send all data in a loop (TCP may not send everything at once)
    while (TotalBytesSent < TotalDataSize)
    {
        int32 BytesSent = 0;
        if (!Client->Send(DataToSend + TotalBytesSent, TotalDataSize - TotalBytesSent, BytesSent))
        {
            UE_LOG(LogUnrealMCP, Error, TEXT("MCPServerRunnable: Failed to send response after %d/%d bytes"),
                   TotalBytesSent, TotalDataSize);
            return;
        }

        TotalBytesSent += BytesSent;
        UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Sent %d bytes (%d/%d total)"),
               BytesSent, TotalBytesSent, TotalDataSize);
    }

    UE_LOG(LogUnrealMCP, Verbose, TEXT("MCPServerRunnable: Response sent successfully (%d bytes)"),
           TotalBytesSent);
} 
