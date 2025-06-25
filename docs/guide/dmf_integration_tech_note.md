# DMF Integration Tech Note

## Overview

This technical note describes the integration approach for incorporating dm-framework (dmf) into the current bbf design while maintaining compatibility with existing implementations.

## Design Principles

### 1. Independence of bbfdm
- bbfdm remains unchanged and independent
- Existing functionality and interfaces are preserved
- No modifications to core bbfdm architecture

### 2. DMF as a Unique Micro Service
- DMF operates as a distinctive micro service with different implementation approach
- Provides the same interface to bbfdmd as existing micro services
- **Exception**: Path resolution feature in bbfdm will not be used as it's not applicable to DMF

### 3. Coexistence Strategy
- Both legacy and DMF-based approaches can coexist
- Bridge manager will be reimplemented using DMF as proof of concept (PoC)

## Architecture Considerations

### Path Reference Limitations

Due to different implementations of path references between DMF and existing solutions, there are important constraints:

- **Restriction**: Different micro services with path references cannot mix implementation approaches
- **Example**: If network module implements `IP.Interface` and DHCP module references `IP.Interface`, they must both use either:
  - Legacy micro service approach, OR
  - DMF-based micro service approach

### Bridge Module PoC
The initial bridge module implementation will:
- Not contain path references to other modules
- Avoid the coexistence limitation mentioned above
- Serve as a proof of concept for DMF integration

## Implementation Details

### 1. bbf_config Adaptation

For DMF-based micro services, bbf_config requires minimal adaptation:
- `commit/revert/reload` operations are already implemented within DMF
- bbf_config simply forwards these operations to the respective DMF service
- No complex logic changes required

### 2. DMF Service Architecture

#### Current DMF Design
- DMF operates as an independent daemon program
- Single daemon serves all data models
- All data model implementations reside within DMF

#### PoC Approach
- Use DMF directly as a large micro service
- Implement only bridge-related data models initially
- Future consideration: Convert DMF to a library supporting multiple independent micro services for different data models

### 3. Configuration Changes

#### Micro Service JSON Definition
Add a new option in micro service definition JSON files:
```json
{
    "daemon": {
        "service_name": "bridge-manager",
        "dmf_service": true,
    }
  // ... other configuration
}
```

#### Service Initialization
Modify `/etc/init.d/bbfdm.services`:
```bash
# Check if service is DMF-based
if [ "$dmf_service" = "true" ]; then
    # Call DMF service
    start_dmf_service "$service_name"
else
    # Use legacy approach
    start_legacy_service "$service_name"
fi
```

### 4. Repository Structure

#### New Repository
- Create `dm-framework` repository in bbf group
- Keep it independent from existing `bbfdm` repository
- Include only bridge-related implementations initially


## Migration Path

### Phase 1: PoC Implementation
1. Set up dm-framework repository
2. Implement bridge manager using DMF
3. Modify bbfdm service initialization
4. Test coexistence with existing micro services

### Phase 2: Enhanced Integration
1. Evaluate PoC results
2. Consider converting DMF to library approach
3. Implement additional data models if successful


## Conclusion

This integration approach provides a safe and manageable path for incorporating DMF into the BBF ecosystem. The coexistence strategy allows for gradual adoption while maintaining system stability and functionality.

The bridge manager PoC will serve as a valuable proof point for the viability of this approach and inform future decisions about broader DMF adoption. 