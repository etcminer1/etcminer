# Pool Examples for etcminer

Pool connection definition is issued via `-P` argument which has this syntax:

```
-P scheme://user[.workername][:password]@hostname:port[/...]
```
__values in square brackets are optional__

where `scheme` can be any of:

* `http` for getwork mode (geth)
* `stratum+tcp` for plain stratum mode
* `stratum1+tcp` for plain stratum eth-proxy compatible mode, note that eth-proxy still works for etcminer
* `stratum2+tcp` for plain stratum NiceHash compatible mode

Please try different schemes if you are not sure which scheme is supported by the pool.

## Format details

```
                                              Authority
            +---------------------------------------------------------------------------------+
  stratum://0x23413a007da796875efa2f8c98fcc011c247f023.Worker:password@etc.certainpool.com:4444
  +------+  +--------------------------------------------------------+ +-----------------+ +--+
      |                             |                                           |            |
      |                             |                                           |            + > Port
      |                             |                                           + ------------ > Host
      |                             + -------------------------------------------------------- > User Info
      + -------------------------------------------------------------------------------------- > Scheme
      
```