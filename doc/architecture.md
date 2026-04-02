# SONiC SWSS Architecture

## Tài liệu liên quan

- Thiết kế chi tiết OrchAgent và các Orch liên quan: `doc/orchagent-detailed-design.md`

## Overview
Tài liệu này mô tả kiến trúc của SONiC Switch State Service (SWSS).

## Main Architecture Diagram

```mermaid
graph TB
    subgraph "OrchDaemon Container"
        OD[OrchDaemon<br/>Main Coordinator]
        OL[m_orchList<br/>vector&lt;Orch*&gt;]
        SEL[Select Dispatcher<br/>Event Loop Manager]
    end
    
    subgraph "Base Framework"
        ORCH[Orch<br/>Base Class]
        EXEC[Executor<br/>Abstract Base]
        CONS[Consumer<br/>DB Event Handler]
        SEL_IF[Selectable<br/>Interface]
    end
    
    subgraph "Core Orchestrators"
        SO[SwitchOrch<br/>Switch Global Config]
        PO[PortsOrch<br/>Port/VLAN/LAG Mgmt]
        RO[RouteOrch<br/>IP Routing]
        NO[NeighOrch<br/>ARP/ND Table]
        FO[FdbOrch<br/>MAC Learning]
        IO[IntfsOrch<br/>L3 Interfaces]
        VO[VrfOrch<br/>Virtual Routing]
    end
    
    subgraph "QoS & Buffer"
        QO[QosOrch<br/>QoS Policies]
        BO[BufferOrch<br/>Buffer Management]
        PL[PolicerOrch<br/>Rate Limiting]
    end
    
    subgraph "Security & ACL"
        AO[AclOrch<br/>Access Control]
        MO[MirrorOrch<br/>Port Mirroring]
        CO[CrmOrch<br/>Resource Monitor]
    end
    
    subgraph "Redis Databases"
        APPL[(APPL_DB<br/>Application Data)]
        CONFIG[(CONFIG_DB<br/>Configuration)]
        STATE[(STATE_DB<br/>Operational State)]
        ASIC[(ASIC_DB<br/>Hardware State)]
        COUNTERS[(COUNTERS_DB<br/>Statistics)]
    end
    
    subgraph "Hardware Layer"
        SAI[SAI API<br/>Switch Abstraction]
        HW[Switch Hardware<br/>ASIC/NPU]
    end
    
    %% Main relationships
    OD --> OL
    OD --> SEL
    OL --> SO
    OL --> PO
    OL --> RO
    OL --> NO
    OL --> FO
    OL --> IO
    OL --> VO
    OL --> QO
    OL --> BO
    OL --> PL
    OL --> AO
    OL --> MO
    OL --> CO
    
    %% Inheritance
    SO --> ORCH
    PO --> ORCH
    RO --> ORCH
    NO --> ORCH
    FO --> ORCH
    IO --> ORCH
    VO --> ORCH
    QO --> ORCH
    BO --> ORCH
    PL --> ORCH
    AO --> ORCH
    MO --> ORCH
    CO --> ORCH
    
    ORCH --> EXEC
    EXEC --> CONS
    CONS --> SEL_IF
    
    %% Database connections
    SEL_IF -.-> APPL
    SEL_IF -.-> CONFIG
    SEL_IF -.-> STATE
    
    %% Hardware connections
    ORCH --> SAI
    SAI --> HW
    SAI -.-> ASIC
    
    %% Styling
    classDef daemon fill:#e1f5fe,stroke:#01579b,stroke-width:2px
    classDef orch fill:#e8f5e8,stroke:#1b5e20,stroke-width:2px
    classDef db fill:#fff3e0,stroke:#e65100,stroke-width:2px
    classDef hw fill:#f3e5f5,stroke:#4a148c,stroke-width:2px
    classDef framework fill:#fce4ec,stroke:#880e4f,stroke-width:2px
    
    class OD,OL,SEL daemon
    class SO,PO,RO,NO,FO,IO,VO,QO,BO,PL,AO,MO,CO orch
    class APPL,CONFIG,STATE,ASIC,COUNTERS db
    class SAI,HW hw
    class ORCH,EXEC,CONS,SEL_IF framework
```

## Class Hierarchy

```mermaid
classDiagram
    class OrchDaemon {
        -vector~Orch*~ m_orchList
        -Select* m_select
        -DBConnector* m_applDb
        -DBConnector* m_configDb
        -DBConnector* m_stateDb
        +bool init()
        +void start(long heartBeatInterval)
        +bool warmRestartCheck()
        -void flush()
        -void heartBeat()
    }
    
    class Orch {
        <<abstract>>
        #ConsumerMap m_consumerMap
        #DBConnector* m_db
        #string m_name
        +virtual void doTask()
        +virtual void doTask(Consumer& consumer)
        +vector~Selectable*~ getSelectables()
        +void addExecutor(Executor* executor)
        +string getName()
    }
    
    class PortsOrch {
        -map~string,Port~ m_portList
        -map~string,Vlan~ m_vlanList
        -map~string,Lag~ m_lagList
        +void doTask()
        +void doTask(Consumer& consumer)
        +void doPortTask(Consumer& consumer)
        +void doVlanTask(Consumer& consumer)
        +void doLagTask(Consumer& consumer)
        +bool setPortSpeed(Port& port, uint32_t speed)
        +bool setPortAdminStatus(Port& port, bool up)
        +bool addVlan(string vlan_alias)
    }
    
    class RouteOrch {
        -IpPrefix2NextHopGroupId m_routes
        -NextHopGroupTable m_nhgTable
        +void doTask()
        +void doTask(Consumer& consumer)
        +bool addRoute(IpPrefix prefix, NextHopGroup nhg)
        +bool removeRoute(IpPrefix prefix)
        +NextHopGroupId getNextHopGroupId(NextHopGroup nhg)
    }
    
    class Consumer {
        +SyncMap m_toSync
        +string m_tableName
        +void execute()
        +void drain()
        +ConsumerTableBase* getConsumerTable()
        +void addToSync(string key, tuple data)
    }
    
    class Executor {
        <<abstract>>
        #Selectable* m_selectable
        #Orch* m_orch
        #string m_name
        +virtual void execute()
        +virtual void drain()
        +Selectable* getSelectable()
        +string getName()
    }
    
    OrchDaemon *-- Orch : manages
    PortsOrch --|> Orch : inherits
    RouteOrch --|> Orch : inherits
    Orch *-- Executor : contains
    Consumer --|> Executor : inherits
```

## Event Processing Flow

```mermaid
sequenceDiagram
    participant App as Application/CLI
    participant CDB as CONFIG_DB
    participant ADB as APPL_DB
    participant OD as OrchDaemon
    participant PO as PortsOrch
    participant SAI as SAI API
    participant HW as Hardware
    participant SDB as STATE_DB
    
    Note over App,SDB: Port Configuration Example
    
    App->>CDB: config interface Ethernet0 speed 100000
    CDB->>ADB: PORT_TABLE update
    ADB->>OD: Redis notification
    OD->>OD: select() detects event
    OD->>PO: Consumer::execute()
    PO->>ADB: pop() data from table
    ADB-->>PO: key="Ethernet0", fields={"speed":"100000"}
    PO->>PO: addToSync()
    PO->>PO: doTask()
    PO->>PO: doPortTask()
    PO->>SAI: sai_port_api->set_port_attribute()
    SAI->>HW: Configure port speed
    HW-->>SAI: Success/Failure
    SAI-->>PO: Return status
    PO->>SDB: Update PORT_TABLE operational status
    PO->>PO: notify observers (if any)
```

## Database Schema Integration

```mermaid
erDiagram
    CONFIG_DB {
        string PORT_table "Port configuration"
        string VLAN_table "VLAN configuration" 
        string LAG_table "LAG configuration"
        string ACL_table "ACL configuration"
    }
    
    APPL_DB {
        string PORT_TABLE "Runtime port config"
        string ROUTE_TABLE "IP routes"
        string NEIGH_TABLE "ARP/ND entries"
        string FDB_TABLE "MAC addresses"
        string ACL_RULE_TABLE "ACL rules"
    }
    
    STATE_DB {
        string PORT_TABLE "Port operational state"
        string LAG_TABLE "LAG operational state"
        string VLAN_TABLE "VLAN operational state"
    }
    
    ASIC_DB {
        string SAI_OBJECT "SAI object states"
        string ASIC_STATE "Hardware states"
    }
    
    CONFIG_DB ||--|| APPL_DB : "configmgr processes"
    APPL_DB ||--|| STATE_DB : "orchagent updates"
    APPL_DB ||--|| ASIC_DB : "SAI operations"
```