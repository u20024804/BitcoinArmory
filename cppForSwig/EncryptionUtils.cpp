////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2011-2012, Alan C. Reiner    <alan.reiner@gmail.com>        //
//  Distributed under the GNU Affero General Public License (AGPL v3)         //
//  See LICENSE or http://www.gnu.org/licenses/agpl.html                      //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
#include "EncryptionUtils.h"
#include "integer.h"
#include "oids.h"


#define CRYPTO_DEBUG false
#define LITTLE_ENDIAN_SYS 1


/////////////////////////////////////////////////////////////////////////////
// We have to explicitly re-define some of these methods...
SecureBinaryData & SecureBinaryData::append(SecureBinaryData & sbd2) 
{
   if(sbd2.getSize()==0) 
      return (*this);

   if(getSize()==0) 
      BinaryData::copyFrom(sbd2.getPtr(), sbd2.getSize());
   else
      BinaryData::append(sbd2.getRawRef());

   lockData();
   return (*this);
}


/////////////////////////////////////////////////////////////////////////////
SecureBinaryData & SecureBinaryData::append(BinaryData & bd) 
{
   if(bd.getSize()==0) 
      return (*this);

   if(getSize()==0) 
      BinaryData::copyFrom(bd.getPtr(), bd.getSize());
   else
      BinaryData::append(bd.getRef());

   lockData();
   return (*this);
}


/////////////////////////////////////////////////////////////////////////////
SecureBinaryData SecureBinaryData::operator+(SecureBinaryData & sbd2) const
{
   SecureBinaryData out(getSize() + sbd2.getSize());
   memcpy(out.getPtr(), getPtr(), getSize());
   memcpy(out.getPtr()+getSize(), sbd2.getPtr(), sbd2.getSize());
   out.lockData();
   return out;
}

/////////////////////////////////////////////////////////////////////////////
SecureBinaryData & SecureBinaryData::operator=(SecureBinaryData const & sbd2)
{ 
   copyFrom(sbd2.getPtr(), sbd2.getSize() );
   lockData(); 
   return (*this);
}

/////////////////////////////////////////////////////////////////////////////
bool SecureBinaryData::operator==(SecureBinaryData const & sbd2) const
{ 
   if(getSize() != sbd2.getSize())
      return false;
   for(unsigned int i=0; i<getSize(); i++)
      if( (*this)[i] != sbd2[i] )
         return false;
   return true;
}

/////////////////////////////////////////////////////////////////////////////
// Swap endianness of the bytes in the index range [pos1, pos2)
SecureBinaryData SecureBinaryData::copySwapEndian(size_t pos1, size_t pos2) const
{
   return SecureBinaryData(BinaryData::copySwapEndian(pos1, pos2));
}

/////////////////////////////////////////////////////////////////////////////
SecureBinaryData SecureBinaryData::GenerateRandom(uint32_t numBytes)
{
   static CryptoPP::AutoSeededRandomPool prng;
   SecureBinaryData randData(numBytes);
   prng.GenerateBlock(randData.getPtr(), numBytes);
   return randData;  
}

/////////////////////////////////////////////////////////////////////////////
KdfRomix::KdfRomix(void) : 
   hashFunctionName_( "sha512" ),
   hashOutputBytes_( 64 ),
   kdfOutputBytes_( 32 ),
   memoryReqtBytes_( 32 ),
   numIterations_( 0 )
{ 
   // Nothing to do here
}

/////////////////////////////////////////////////////////////////////////////
KdfRomix::KdfRomix(uint32_t memReqts, uint32_t numIter, SecureBinaryData salt) :
   hashFunctionName_( "sha512" ),
   hashOutputBytes_( 64 ),
   kdfOutputBytes_( 32 )
{
   usePrecomputedKdfParams(memReqts, numIter, salt);
}

/////////////////////////////////////////////////////////////////////////////
void KdfRomix::computeKdfParams(double targetComputeSec, uint32_t maxMemReqts)
{
   // Create a random salt, even though this is probably unnecessary:
   // the variation in numIter and memReqts is probably effective enough
   salt_ = SecureBinaryData().GenerateRandom(16);

   // If target compute is 0s, then this method really only generates 
   // a random salt, and sets the other params to default minimum.
   if(targetComputeSec == 0)
   {
      numIterations_ = 1;
      memoryReqtBytes_ = DEFAULT_KDF_MIN_MEMORY;
      return;
   }


   // Here, we pick the largest memory reqt that allows the executing system
   // to compute the KDF in less than the target time.  A maximum can be 
   // specified, in case the target system is likely to be memory-limited
   // more than compute-speed limited
   SecureBinaryData testKey("This is an example key to test KDF iteration speed");

   // Start the search for a memory value at 1kB
   memoryReqtBytes_ = DEFAULT_KDF_MIN_MEMORY;
   double approxSec = 0;
   while(approxSec <= targetComputeSec/4 && memoryReqtBytes_ < maxMemReqts)
   {
      memoryReqtBytes_ *= 2;

      sequenceCount_ = memoryReqtBytes_ / hashOutputBytes_;
      lookupTable_.resize(memoryReqtBytes_);

      TIMER_RESTART("KDF_Mem_Search");
      testKey = DeriveKey_OneIter(testKey);
      TIMER_STOP("KDF_Mem_Search");
      approxSec = TIMER_READ_SEC("KDF_Mem_Search");
   }

   // Recompute here, in case we didn't enter the search above 
   sequenceCount_ = memoryReqtBytes_ / hashOutputBytes_;
   lookupTable_.resize(memoryReqtBytes_);


   // Depending on the search above (or if a low max memory was chosen, 
   // we may need to do multiple iterations to achieve the desired compute
   // time on this system.
   double allItersSec = 0;
   uint32_t numTest = 1;
   while(allItersSec < 0.02)
   {
      numTest *= 2;
      TIMER_RESTART("KDF_Time_Search");
      for(uint32_t i=0; i<numTest; i++)
      {
         SecureBinaryData testKey("This is an example key to test KDF iteration speed");
         testKey = DeriveKey_OneIter(testKey);
      }
      TIMER_STOP("KDF_Time_Search");
      allItersSec = TIMER_READ_SEC("KDF_Time_Search");
   }

   double perIterSec  = allItersSec / numTest;
   numIterations_ = (uint32_t)(targetComputeSec / (perIterSec+0.0005));
   numIterations_ = (numIterations_ < 1 ? 1 : numIterations_);
   //cout << "System speed test results    :  " << endl;
   //cout << "   Total test of the KDF took:  " << allItersSec*1000 << " ms" << endl;
   //cout << "                   to execute:  " << numTest << " iterations" << endl;
   //cout << "   Target computation time is:  " << targetComputeSec*1000 << " ms" << endl;
   //cout << "   Setting numIterations to:    " << numIterations_ << endl;
}



/////////////////////////////////////////////////////////////////////////////
void KdfRomix::usePrecomputedKdfParams(uint32_t memReqts, 
                                       uint32_t numIter, 
                                       SecureBinaryData salt)
{
   memoryReqtBytes_ = memReqts;
   sequenceCount_   = memoryReqtBytes_ / hashOutputBytes_;
   numIterations_   = numIter;
   salt_            = salt;
}

/////////////////////////////////////////////////////////////////////////////
void KdfRomix::printKdfParams(void)
{
   // SHA512 computes 64-byte outputs
   cout << "KDF Parameters:" << endl;
   cout << "   HashFunction : " << hashFunctionName_ << endl;
   cout << "   HashOutBytes : " << hashOutputBytes_ << endl;
   cout << "   Memory/thread: " << memoryReqtBytes_ << " bytes" << endl;
   cout << "   SequenceCount: " << sequenceCount_   << endl;
   cout << "   NumIterations: " << numIterations_   << endl;
   cout << "   KDFOutBytes  : " << kdfOutputBytes_  << endl;
   cout << "   Salt         : " << salt_.toHexStr() << endl;
}


/////////////////////////////////////////////////////////////////////////////
SecureBinaryData KdfRomix::DeriveKey_OneIter(SecureBinaryData const & password)
{
   static CryptoPP::SHA512 sha512;

   // Concatenate the salt/IV to the password
   SecureBinaryData saltedPassword = password + salt_; 
   
   // Prepare the lookup table
   lookupTable_.resize(memoryReqtBytes_);
   lookupTable_.fill(0);
   uint32_t const HSZ = hashOutputBytes_;
   uint8_t* frontOfLUT = lookupTable_.getPtr();
   uint8_t* nextRead  = NULL;
   uint8_t* nextWrite = NULL;

   // First hash to seed the lookup table, input is variable length anyway
   sha512.CalculateDigest(frontOfLUT, 
                          saltedPassword.getPtr(), 
                          saltedPassword.getSize());

   // Compute <sequenceCount_> consecutive hashes of the passphrase
   // Every iteration is stored in the next 64-bytes in the Lookup table
   for(uint32_t nByte=0; nByte<memoryReqtBytes_-HSZ; nByte+=HSZ)
   {
      // Compute hash of slot i, put result in slot i+1
      nextRead  = frontOfLUT + nByte;
      nextWrite = nextRead + hashOutputBytes_;
      sha512.CalculateDigest(nextWrite, nextRead, hashOutputBytes_);
   }

   // LookupTable should be complete, now start lookup sequence.
   // Start with the last hash from the previous step
   SecureBinaryData X(frontOfLUT + memoryReqtBytes_ - HSZ, HSZ);
   SecureBinaryData Y(HSZ);

   // We "integerize" a hash value by taking the last 4 bytes of
   // as a uint32_t, and take modulo sequenceCount
   uint64_t* X64ptr = (uint64_t*)(X.getPtr());
   uint64_t* Y64ptr = (uint64_t*)(Y.getPtr());
   uint64_t* V64ptr = NULL;
   uint32_t newIndex;
   uint32_t const nXorOps = HSZ/sizeof(uint64_t);

   // Pure ROMix would use sequenceCount_ for the number of lookups.
   // We divide by 2 to reduce computation time RELATIVE to the memory usage
   // This still provides suffient LUT operations, but allows us to use more
   // memory in the same amount of time (and this is the justification for
   // the scrypt algorithm -- it is basically ROMix, modified for more 
   // flexibility in controlling compute-time vs memory-usage).
   uint32_t const nLookups = sequenceCount_ / 2;
   for(uint32_t nSeq=0; nSeq<nLookups; nSeq++)
   {
      // Interpret last 4 bytes of last result (mod seqCt) as next LUT index
      newIndex = *(uint32_t*)(X.getPtr()+HSZ-4) % sequenceCount_;

      // V represents the hash result at <newIndex>
      V64ptr = (uint64_t*)(frontOfLUT + HSZ*newIndex);

      // xor X with V, and store the result in X
      for(uint32_t i=0; i<nXorOps; i++)
         *(Y64ptr+i) = *(X64ptr+i) ^ *(V64ptr+i);

      // Hash the xor'd data to get the next index for lookup
      sha512.CalculateDigest(X.getPtr(), Y.getPtr(), HSZ);
   }
   // Truncate the final result to get the final key
   lookupTable_.destroy();
   return X.getSliceCopy(0,kdfOutputBytes_);
}

/////////////////////////////////////////////////////////////////////////////
SecureBinaryData KdfRomix::DeriveKey(SecureBinaryData const & password)
{
   SecureBinaryData masterKey(password);
   for(uint32_t i=0; i<numIterations_; i++)
      masterKey = DeriveKey_OneIter(masterKey);
   
   return SecureBinaryData(masterKey);
}





/////////////////////////////////////////////////////////////////////////////
// Implement AES encryption using AES mode, CFB
SecureBinaryData CryptoAES::EncryptCFB(SecureBinaryData & data, 
                                       SecureBinaryData & key,
                                       SecureBinaryData & iv)
{
   if(CRYPTO_DEBUG)
   {
      cout << "AES Decrypt" << endl;
      cout << "   BinData: " << data.toHexStr() << endl;
      cout << "   BinKey : " << key.toHexStr() << endl;
      cout << "   BinIV  : " << iv.toHexStr() << endl;
   }


   if(data.getSize() == 0)
      return SecureBinaryData(0);

   SecureBinaryData encrData(data.getSize());

   // Caller can supply their own IV/entropy, or let it be generated here
   // (variable "iv" is a reference, so check it on the way out)
   if(iv.getSize() == 0)
      iv = SecureBinaryData().GenerateRandom(CRYPTO_AES::BLOCKSIZE);


   CRYPTO_CFB_MODE<CRYPTO_AES>::Encryption aes_enc( (byte*)key.getPtr(), 
                                                     key.getSize(), 
                                              (byte*)iv.getPtr());

   aes_enc.ProcessData( (byte*)encrData.getPtr(), 
                        (byte*)data.getPtr(), 
                               data.getSize());

   return encrData;
}

/////////////////////////////////////////////////////////////////////////////
// Implement AES decryption using AES mode, CFB
SecureBinaryData CryptoAES::DecryptCFB(SecureBinaryData & data, 
                                       SecureBinaryData & key,
                                       SecureBinaryData   iv  )
{
   if(CRYPTO_DEBUG)
   {
      cout << "AES Decrypt" << endl;
      cout << "   BinData: " << data.toHexStr() << endl;
      cout << "   BinKey : " << key.toHexStr() << endl;
      cout << "   BinIV  : " << iv.toHexStr() << endl;
   }


   if(data.getSize() == 0)
      return SecureBinaryData(0);

   SecureBinaryData unencrData(data.getSize());

   CRYPTO_CFB_MODE<CRYPTO_AES>::Decryption aes_enc( (byte*)key.getPtr(), 
                                                     key.getSize(), 
                                              (byte*)iv.getPtr());

   aes_enc.ProcessData( (byte*)unencrData.getPtr(), 
                        (byte*)data.getPtr(), 
                               data.getSize());

   return unencrData;
}



/////////////////////////////////////////////////////////////////////////////
// Same as above, but only changing the AES mode of operation (CBC, not CFB)
SecureBinaryData CryptoAES::EncryptCBC(SecureBinaryData & data, 
                                       SecureBinaryData & key,
                                       SecureBinaryData & iv)
{
   if(CRYPTO_DEBUG)
   {
      cout << "AES Decrypt" << endl;
      cout << "   BinData: " << data.toHexStr() << endl;
      cout << "   BinKey : " << key.toHexStr() << endl;
      cout << "   BinIV  : " << iv.toHexStr() << endl;
   }

   if(data.getSize() == 0)
      return SecureBinaryData(0);

   SecureBinaryData encrData(data.getSize());

   // Caller can supply their own IV/entropy, or let it be generated here
   // (variable "iv" is a reference, so check it on the way out)
   if(iv.getSize() == 0)
      iv = SecureBinaryData().GenerateRandom(CRYPTO_AES::BLOCKSIZE);


   CRYPTO_CBC_MODE<CRYPTO_AES>::Encryption aes_enc( (byte*)key.getPtr(), 
                                                     key.getSize(), 
                                              (byte*)iv.getPtr());

   aes_enc.ProcessData( (byte*)encrData.getPtr(), 
                        (byte*)data.getPtr(), 
                               data.getSize());

   return encrData;
}

/////////////////////////////////////////////////////////////////////////////
// Same as above, but only changing the AES mode of operation (CBC, not CFB)
SecureBinaryData CryptoAES::DecryptCBC(SecureBinaryData & data, 
                                       SecureBinaryData & key,
                                       SecureBinaryData   iv  )
{
   if(CRYPTO_DEBUG)
   {
      cout << "AES Decrypt" << endl;
      cout << "   BinData: " << data.toHexStr() << endl;
      cout << "   BinKey : " << key.toHexStr() << endl;
      cout << "   BinIV  : " << iv.toHexStr() << endl;
   }

   if(data.getSize() == 0)
      return SecureBinaryData(0);

   SecureBinaryData unencrData(data.getSize());

   CRYPTO_CBC_MODE<CRYPTO_AES>::Decryption aes_enc( (byte*)key.getPtr(), 
                                                     key.getSize(), 
                                              (byte*)iv.getPtr());

   aes_enc.ProcessData( (byte*)unencrData.getPtr(), 
                        (byte*)data.getPtr(), 
                               data.getSize());
   return unencrData;
}




/////////////////////////////////////////////////////////////////////////////
EC_PRIVKEY CryptoECDSA::CreateNewPrivateKey(void)
{
   return ParsePrivateKey(SecureBinaryData().GenerateRandom(32));
}

/////////////////////////////////////////////////////////////////////////////
EC_PRIVKEY CryptoECDSA::ParsePrivateKey(SecureBinaryData const & privKeyData)
{
   EC_PRIVKEY cppPrivKey;

   // Priv key can actually have an extra 0x01 byte, that's why we hard-code
   // the "32" below instead of using .getSize()
   CryptoPP::Integer privateExp;
   privateExp.Decode(privKeyData.getPtr(), 32, UNSIGNED);
   cppPrivKey.Initialize(EC_CURVE, privateExp);
   return cppPrivKey;
}


/////////////////////////////////////////////////////////////////////////////
EC_PUBKEY CryptoECDSA::ParsePublicKey(SecureBinaryData const & pubKey33or65)
{
   CryptoPP::ECP & ecp = Get_secp256k1_ECP();
   EC_PUBKEY btcPubKey;
   EC_POINT pubPt;
   ecp.DecodePoint(pubPt, (byte*)pubKey33or65.getPtr(), pubKey33or65.getSize());

   btcPubKey.Initialize(EC_CURVE, pubPt);
   return btcPubKey;
}


/////////////////////////////////////////////////////////////////////////////
SecureBinaryData CryptoECDSA::SerializePrivateKey(EC_PRIVKEY const & privKey)
{
   CryptoPP::Integer privateExp = privKey.GetPrivateExponent();
   SecureBinaryData privKeyData(32);
   privateExp.Encode(privKeyData.getPtr(), privKeyData.getSize(), UNSIGNED);
   return privKeyData;
}
   
/////////////////////////////////////////////////////////////////////////////
SecureBinaryData CryptoECDSA::SerializePublicKey(EC_PUBKEY const & pubKey,
                                                 bool doCompress)
{
   CryptoPP::ECP & ecp = Get_secp256k1_ECP();
   EC_POINT publicPoint = pubKey.GetPublicElement();
   if(doCompress)
   {
      SecureBinaryData pubData(33);
      ecp.EncodePoint((byte*)pubData.getPtr(), publicPoint, true);
      return pubData;
   }
   else
   {
      SecureBinaryData pubData(65);
      ecp.EncodePoint((byte*)pubData.getPtr(), publicPoint, false);
      return pubData;
   }
}

/////////////////////////////////////////////////////////////////////////////
SecureBinaryData CryptoECDSA::ComputePublicKey(SecureBinaryData const & cppPrivKey,
                                               bool doCompress)
{
   EC_PRIVKEY pk = ParsePrivateKey(cppPrivKey);
   EC_PUBKEY  pub;
   pk.MakePublicKey(pub);
   return SerializePublicKey(pub, doCompress);
}

/////////////////////////////////////////////////////////////////////////////
EC_PUBKEY CryptoECDSA::ComputePublicKey(EC_PRIVKEY const & cppPrivKey)
{
   EC_PUBKEY cppPubKey;
   cppPrivKey.MakePublicKey(cppPubKey);

   // Validate the public key -- not sure why this needs a prng...
   static CRYPTO_PRNG prng;
   assert(cppPubKey.Validate(prng, 3));

   return cppPubKey;
}

////////////////////////////////////////////////////////////////////////////////
SecureBinaryData CryptoECDSA::GenerateNewPrivateKey(void)
{
   return SecureBinaryData().GenerateRandom(32);
}


////////////////////////////////////////////////////////////////////////////////
bool CryptoECDSA::CheckPubPrivKeyMatch(EC_PRIVKEY const & cppPrivKey,
                                       EC_PUBKEY  const & cppPubKey)
{
   EC_PUBKEY computedPubKey;
   cppPrivKey.MakePublicKey(computedPubKey);
   
   EC_POINT ppA = cppPubKey.GetPublicElement();
   EC_POINT ppB = computedPubKey.GetPublicElement();
   return (ppA.x==ppB.x && ppA.y==ppB.y);
}

/////////////////////////////////////////////////////////////////////////////
bool CryptoECDSA::CheckPubPrivKeyMatch(SecureBinaryData const & privKey32or33,
                                       SecureBinaryData const & pubKey33or65)
{
   if(CRYPTO_DEBUG)
   {
      cout << "CheckPubPrivKeyMatch:" << endl;
      cout << "   BinPrv: " << privKey32or33.toHexStr() << endl;
      cout << "   BinPub: " << pubKey33or65.toHexStr() << endl;
   }

   EC_PRIVKEY privKey = ParsePrivateKey(privKey32or33);
   EC_PUBKEY  pubKey  = ParsePublicKey(pubKey33or65);
   return CheckPubPrivKeyMatch(privKey, pubKey);
}

/////////////////////////////////////////////////////////////////////////////
bool CryptoECDSA::VerifyPublicKeyValid(SecureBinaryData const & pubKey33or65)
{
   CryptoPP::ECP & ecp = Get_secp256k1_ECP();

   if(CRYPTO_DEBUG)
      cout << "BinPub: " << pubKey33or65.toHexStr() << endl;

   EC_PUBKEY btcPubKey;
   EC_POINT pubPt;
   ecp.DecodePoint(pubPt, (byte*)pubKey33or65.getPtr(), 
                                   pubKey33or65.getSize());

   btcPubKey.Initialize(EC_CURVE, pubPt);

   // Validate the public key -- not sure why this needs a PRNG
   static CRYPTO_PRNG prng;
   return btcPubKey.Validate(prng, 3);
}

/////////////////////////////////////////////////////////////////////////////
SecureBinaryData CryptoECDSA::SignData(SecureBinaryData const & binToSign, 
                                       SecureBinaryData const & binPrivKey)
{
   if(CRYPTO_DEBUG)
   {
      cout << "SignData:" << endl;
      cout << "   BinSgn: " << binToSign.getSize() << " " << binToSign.toHexStr() << endl;
      cout << "   BinPrv: " << binPrivKey.getSize() << " " << binPrivKey.toHexStr() << endl;
   }
   EC_PRIVKEY cppPrivKey = ParsePrivateKey(binPrivKey);
   return SignData(binToSign, cppPrivKey);
}

/////////////////////////////////////////////////////////////////////////////
SecureBinaryData CryptoECDSA::SignData(SecureBinaryData const & binToSign, 
                                       EC_PRIVKEY const & cppPrivKey)
{

   // We trick the Crypto++ ECDSA module by passing it a single-hashed
   // message, it will do the second hash before it signs it.  This is 
   // exactly what we need.
   static CryptoPP::SHA256  sha256;
   static CRYPTO_PRNG prng;

   // Execute the first sha256 op -- the signer will do the other one
   SecureBinaryData hashVal(32);
   sha256.CalculateDigest(hashVal.getPtr(), 
                          binToSign.getPtr(), 
                          binToSign.getSize());

   string signature;
   EC_SIGNER signer(cppPrivKey);
   CryptoPP::StringSource(
               hashVal.toBinStr(), true, new CryptoPP::SignerFilter(
               prng, signer, new CryptoPP::StringSink(signature))); 
  
   return SecureBinaryData(signature);
}


/////////////////////////////////////////////////////////////////////////////
bool CryptoECDSA::VerifyData(SecureBinaryData const & binMessage, 
                             SecureBinaryData const & binSignature,
                             SecureBinaryData const & pubkey65B)
{
   if(CRYPTO_DEBUG)
   {
      cout << "VerifyData:" << endl;
      cout << "   BinMsg: " << binMessage.toHexStr() << endl;
      cout << "   BinSig: " << binSignature.toHexStr() << endl;
      cout << "   BinPub: " << pubkey65B.toHexStr() << endl;
   }

   EC_PUBKEY cppPubKey = ParsePublicKey(pubkey65B);
   return VerifyData(binMessage, binSignature, cppPubKey);
}

/////////////////////////////////////////////////////////////////////////////
bool CryptoECDSA::VerifyData(SecureBinaryData const & binMessage, 
                             SecureBinaryData const & binSignature,
                             EC_PUBKEY const & cppPubKey)
                            
{


   static CryptoPP::SHA256  sha256;
   static CryptoPP::AutoSeededRandomPool prng;

   assert(cppPubKey.Validate(prng, 3));

   // We execute the first SHA256 op, here.  Next one is done by Verifier
   SecureBinaryData hashVal(32);
   sha256.CalculateDigest(hashVal.getPtr(), 
                          binMessage.getPtr(), 
                          binMessage.getSize());

   // Verifying message 
   EC_VERIFIER verifier(cppPubKey); 
   return verifier.VerifyMessage((const byte*)hashVal.getPtr(), 
                                              hashVal.getSize(),
                                 (const byte*)binSignature.getPtr(), 
                                              binSignature.getSize());
}



////////////////////////////////////////////////////////////////////////////////
bool CryptoECDSA::ECVerifyPoint(BinaryData const & x,
                                BinaryData const & y)
{
   EC_PUBKEY cppPubKey;

   CryptoPP::Integer pubX;
   CryptoPP::Integer pubY;
   pubX.Decode(x.getPtr(), x.getSize(), UNSIGNED);
   pubY.Decode(y.getPtr(), y.getSize(), UNSIGNED);
   EC_POINT publicPoint(pubX, pubY);

   // Initialize the public key with the ECP point just created
   cppPubKey.Initialize(EC_CURVE, publicPoint);

   // Validate the public key -- not sure why this needs a PRNG
   static CRYPTO_PRNG prng;
   return cppPubKey.Validate(prng, 3);
}


////////////////////////////////////////////////////////////////////////////////
CryptoPP::ECP& CryptoECDSA::Get_secp256k1_ECP(void)
{
   static bool firstRun = true;
   static CryptoPP::ECP theECP;
   if(firstRun) 
   {
      BinaryData N = BinaryData::CreateFromHex(
            "fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f");
      BinaryData a = BinaryData::CreateFromHex(
            "0000000000000000000000000000000000000000000000000000000000000000");
      BinaryData b = BinaryData::CreateFromHex(
           "0000000000000000000000000000000000000000000000000000000000000007");

      CryptoPP::Integer intN, inta, intb;

      intN.Decode( N.getPtr(),  N.getSize(),  UNSIGNED);
      inta.Decode( a.getPtr(),  a.getSize(),  UNSIGNED);
      intb.Decode( b.getPtr(),  b.getSize(),  UNSIGNED);
  
      theECP = CryptoPP::ECP(intN, inta, intb);
   }
   return theECP;
}




////////////////////////////////////////////////////////////////////////////////
BinaryData CryptoECDSA::ECMultiplyScalars(BinaryData const & A, 
                                          BinaryData const & B)
{
   // Hardcode the order of the secp256k1 EC group
   static BinaryData N = BinaryData::CreateFromHex(
           "fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");

   CryptoPP::Integer intA, intB, intC, intN;
   intA.Decode(A.getPtr(), A.getSize(), UNSIGNED);
   intB.Decode(B.getPtr(), B.getSize(), UNSIGNED);
   intN.Decode(N.getPtr(), N.getSize(), UNSIGNED);
   intC = a_times_b_mod_c(intA, intB, intN);

   BinaryData C(32);
   intC.Encode(C.getPtr(), 32, UNSIGNED);
   return C;
}


////////////////////////////////////////////////////////////////////////////////
BinaryData CryptoECDSA::ECAddPoints(BinaryData const & Ax, 
                                    BinaryData const & Ay,
                                    BinaryData const & Bx,
                                    BinaryData const & By)
{
   CryptoPP::ECP ecp = Get_secp256k1_ECP();
   CryptoPP::Integer intAx, intAy, intBx, intBy, intCx, intCy;

   intAx.Decode(Ax.getPtr(), Ax.getSize(), UNSIGNED);
   intAy.Decode(Ay.getPtr(), Ay.getSize(), UNSIGNED);
   intBx.Decode(Bx.getPtr(), Bx.getSize(), UNSIGNED);
   intBy.Decode(By.getPtr(), By.getSize(), UNSIGNED);


   EC_POINT A(intAx, intAy);
   EC_POINT B(intBx, intBy);

   EC_POINT C = ecp.Add(A,B);

   BinaryData Cbd(64);
   C.x.Encode(Cbd.getPtr(),    32, UNSIGNED);
   C.y.Encode(Cbd.getPtr()+32, 32, UNSIGNED);

   return Cbd;
}


////////////////////////////////////////////////////////////////////////////////
BinaryData CryptoECDSA::ECInverse(BinaryData const & Ax, 
                                  BinaryData const & Ay)
                                  
{
   CryptoPP::ECP & ecp = Get_secp256k1_ECP();
   CryptoPP::Integer intAx, intAy, intCx, intCy;

   intAx.Decode(Ax.getPtr(), Ax.getSize(), UNSIGNED);
   intAy.Decode(Ay.getPtr(), Ay.getSize(), UNSIGNED);

   EC_POINT A(intAx, intAy);
   EC_POINT C = ecp.Inverse(A);

   BinaryData Cbd(64);
   C.x.Encode(Cbd.getPtr(),    32, UNSIGNED);
   C.y.Encode(Cbd.getPtr()+32, 32, UNSIGNED);

   return Cbd;
}


////////////////////////////////////////////////////////////////////////////////
SecureBinaryData CryptoECDSA::CompressPoint(SecureBinaryData const & pubKey65)
{
   if(pubKey65.getSize() == 33)
      return pubKey65;  // already compressed

   CryptoPP::ECP & ecp = Get_secp256k1_ECP();
   EC_POINT ptPub;
   ecp.DecodePoint(ptPub, (byte*)pubKey65.getPtr(), 65);
   SecureBinaryData ptCompressed(33);
   ecp.EncodePoint((byte*)ptCompressed.getPtr(), ptPub, true);
   return ptCompressed; 
}

////////////////////////////////////////////////////////////////////////////////
SecureBinaryData CryptoECDSA::UncompressPoint(SecureBinaryData const & pubKey33)
{
   if(pubKey33.getSize() == 65)
      return pubKey33;  // already uncompressed

   CryptoPP::ECP & ecp = Get_secp256k1_ECP();
   EC_POINT ptPub;
   ecp.DecodePoint(ptPub, (byte*)pubKey33.getPtr(), 33);
   SecureBinaryData ptUncompressed(65);
   ecp.EncodePoint((byte*)ptUncompressed.getPtr(), ptPub, false);
   return ptUncompressed; 

}



////////////////////////////////////////////////////////////////////////////////
// This function will eventually replace CryptoPP::ECMultiplyPoint, but I left
// the old one in so I can verify the new one before fully replacing it.
SecureBinaryData CryptoECDSA::ECMultiplyPoint(SecureBinaryData const & scalar, 
                                              SecureBinaryData const & ecPoint,
                                              bool compressOutput)
{
   CryptoPP::ECP ecp = Get_secp256k1_ECP();
   EC_PUBKEY btcPubKey = CryptoECDSA().ParsePublicKey(ecPoint);
   EC_POINT ecPubPt = btcPubKey.GetPublicElement();
   CryptoPP::Integer intScalar;
   intScalar.Decode(scalar.getPtr(), scalar.getSize(), UNSIGNED);
   EC_POINT newPubPt = ecp.ScalarMultiply(ecPubPt, intScalar);

   if(compressOutput)
   {
      SecureBinaryData out(33);
      ecp.EncodePoint((byte*)out.getPtr(), newPubPt, true);
      return out;
   }
   else
   {
      SecureBinaryData out(65);
      ecp.EncodePoint((byte*)out.getPtr(), newPubPt, false);
      return out;
   }

   
}


////////////////////////////////////////////////////////////////////////////////
// This function will eventually replace CryptoPP::ECMultiplyPoint, but I left
// the old one in so I can verify the new one before fully replacing it.
BinaryData CryptoECDSA::ECMultiplyPoint(BinaryData const & scalar, 
                                        BinaryData const & ecPoint,
                                        bool compressOutput)
{
   CryptoPP::ECP ecp = Get_secp256k1_ECP();
   EC_PUBKEY btcPubKey = CryptoECDSA().ParsePublicKey(ecPoint);
   EC_POINT ecPubPt = btcPubKey.GetPublicElement();
   CryptoPP::Integer intScalar;
   intScalar.Decode(scalar.getPtr(), scalar.getSize(), UNSIGNED);
   EC_POINT newPubPt = ecp.ScalarMultiply(ecPubPt, intScalar);

   if(compressOutput)
   {
      BinaryData out(33);
      ecp.EncodePoint((byte*)out.getPtr(), newPubPt, true);
      return out;
   }
   else
   {
      BinaryData out(65);
      ecp.EncodePoint((byte*)out.getPtr(), newPubPt, false);
      return out;
   }

   
}


////////////////////////////////////////////////////////////////////////////////
BinaryData ExtendedKey::getHash160(void) const
{
   if(hasPub())
      return BtcUtils::getHash160(getPub().getPtr(), getPub().getSize());
   else
      return BinaryData(0);
   
}

////////////////////////////////////////////////////////////////////////////////
uint32_t ExtendedKey::getIndex(void) const
{
   if(indicesList_.size() == 0)
      return UINT32_MAX;
   
   list<uint32_t>::const_iterator iter = indicesList_.end();
   iter--;
   return *iter;
}

////////////////////////////////////////////////////////////////////////////////
vector<uint32_t> ExtendedKey::getIndicesVect(void) const
{
   list<uint32_t>::const_iterator iter;
   vector<uint32_t> out(indicesList_.size());
   uint32_t index = 0;
   for(iter=indicesList_.begin(); iter!=indicesList_.end(); iter++)
   {
      out[index] = *iter;
      index++;
   }
   return out;
}
      
////////////////////////////////////////////////////////////////////////////////
ExtendedKey::ExtendedKey(SecureBinaryData const & pr, 
                         SecureBinaryData const & pb, 
                         SecureBinaryData const & ch,
                         SecureBinaryData const & par160,
                         list<uint32_t> parentTreeIdx) :
   privKey_(pr),
   pubKey_(pb),
   chain_(ch),
   parent160_(par160),
   indicesList_(parentTreeIdx)
{
   assert(privKey_.getSize()==0 || privKey_.getSize()==32);
   assert(pubKey_.getSize()==33 || pubKey_.getSize()==65);
   assert(chain_.getSize()==32);
}


////////////////////////////////////////////////////////////////////////////////
ExtendedKey::ExtendedKey(BinaryData const & pub, 
                         BinaryData const & chn,
                         BinaryData const & par160,
                         list<uint32_t> parentTreeIdx) :
   privKey_(0),
   pubKey_(pub),
   chain_(chn),
   parent160_(par160),
   indicesList_(parentTreeIdx)
{
   assert(pubKey_.getSize()==33 || pubKey_.getSize()==65);
   assert(chain_.getSize()==32);
}


////////////////////////////////////////////////////////////////////////////////
// Should be static, but would prevent SWIG from using it.
ExtendedKey ExtendedKey::CreateFromPrivate( 
                               SecureBinaryData const & priv, 
                               SecureBinaryData const & chain,
                               SecureBinaryData const & parent160,
                               list<uint32_t> parentTreeIdx)
{
   ExtendedKey ek;
   ek.privKey_ = priv.copy();
   ek.pubKey_ = CryptoECDSA().ComputePublicKey(ek.privKey_, true);
   ek.chain_ = chain.copy();
   ek.parent160_ = parent160.copy();
   ek.indicesList_ = parentTreeIdx;
   return ek;
}


////////////////////////////////////////////////////////////////////////////////
// Should be static, but would prevent SWIG from using it.
ExtendedKey ExtendedKey::CreateFromPublic( 
                              SecureBinaryData const & pub, 
                              SecureBinaryData const & chain,
                              SecureBinaryData const & parent160,
                              list<uint32_t> parentTreeIdx)
{
   ExtendedKey ek;
   ek.privKey_ = SecureBinaryData(0);
   ek.pubKey_ = pub.copy();
   ek.chain_ = chain.copy();
   ek.parent160_ = parent160.copy();
   ek.indicesList_ = parentTreeIdx;
   return ek;
}



////////////////////////////////////////////////////////////////////////////////
void ExtendedKey::deletePrivateKey(void)
{
   privKey_.destroy(); 
}


////////////////////////////////////////////////////////////////////////////////
ExtendedKey ExtendedKey::makePublicCopy(void)
{
   ExtendedKey ekout = copy();
   ekout.deletePrivateKey();
   return ekout;
}



////////////////////////////////////////////////////////////////////////////////
// Strictly speaking, this isn't necessary, but I want a method in python/SWIG
// that guarantees I'm getting a copy, not a reference
ExtendedKey ExtendedKey::copy(void) const
{
   return ExtendedKey(privKey_, pubKey_, chain_, parent160_, indicesList_);
}



////////////////////////////////////////////////////////////////////////////////
void ExtendedKey::debugPrint(void) const
{
   cout << "Indices:        " << getIndexListString() << endl;
   cout << "Fingerprint:    Self: " << getFingerprint().toHexStr()
        << " Parent: " << getParentHash160().toHexStr() << endl;
   cout << "Private Key:    " << privKey_.toHexStr() << endl;
   cout << "Public Key:   "   << CryptoECDSA().CompressPoint(pubKey_).toHexStr() << endl;
   cout << "Chain Code:     " << chain_.toHexStr() << endl;
   cout << "Hash160:        " << getHash160().toHexStr() << endl << endl;
}




////////////////////////////////////////////////////////////////////////////////
string ExtendedKey::getIndexListString(string prefix) const
{
   stringstream ss;
   ss << prefix;
   vector<uint32_t> indexList = getIndicesVect();
   for(uint32_t i=0; i<indexList.size(); i++)
      ss << "/" << indexList[i]; 
   return ss.str();
}


////////////////////////////////////////////////////////////////////////////////
SecureBinaryData ExtendedKey::getPubCompressed(void) const
{
   return CryptoECDSA().CompressPoint(pubKey_);
}

////////////////////////////////////////////////////////////////////////////////
SecureBinaryData ExtendedKey::getPubUncompressed(void) const
{
   return CryptoECDSA().UncompressPoint(pubKey_);
}


////////////////////////////////////////////////////////////////////////////////
SecureBinaryData HDWalletCrypto::HMAC_SHA512(SecureBinaryData key, 
                                             SecureBinaryData msg)
{
   static uint32_t const BLOCKSIZE  = 128;

   // Reduce large keys via hash-function
   if(key.getSize() > BLOCKSIZE)
      key = BtcUtils::getSHA512(key);


   // Zero-pad smaller keys
   if(key.getSize() < BLOCKSIZE)
   {
      BinaryData zeros = BinaryData(BLOCKSIZE - key.getSize());
      zeros.fill(0x00);
      key.append(zeros);
   }


   SecureBinaryData i_key_pad = SecureBinaryData().XOR( key, 0x36 );
   SecureBinaryData o_key_pad = SecureBinaryData().XOR( key, 0x5c );


   // Inner hash operation
   i_key_pad.append(msg);
   i_key_pad = BtcUtils::getSHA512(i_key_pad);


   // Outer hash operation
   o_key_pad.append(i_key_pad);
   o_key_pad = BtcUtils::getSHA512(o_key_pad);
   

   return o_key_pad;
}


////////////////////////////////////////////////////////////////////////////////
// In the HDWallet gist by Pieter, CKD takes two inputs:
//    1.  Extended Key  (priv/pub key, chaincode)
//    2.  Index n
//
// The ExtendedKey class accommodates full private-included ExtendedKey objects
// or public-key-only.  You can pass in either one here, and it will derive the
// child for whatever key data is there.
//
ExtendedKey HDWalletCrypto::ChildKeyDeriv(ExtendedKey const & extKey, uint32_t n)
{

   // Pad the integer to 4-bytes
   SecureBinaryData binaryN = BtcUtils::uint32ToBinaryBE(n);

   // Can't compute a child with no parent!
   assert(extKey.isInitialized());

   // Make a copy of the public key, make sure it's compressed
   SecureBinaryData comprKey = CryptoECDSA().CompressPoint(extKey.getPub());

   // Append the four-byte index number to the compressed public key
   comprKey.append(binaryN);

   // Apply HMAC to get the child pieces
   SecureBinaryData I = HMAC_SHA512(extKey.getChain(), comprKey);

   // Split the HMAC into the two pieces.
   SecureBinaryData I_left ( I.getPtr(),     32 );
   SecureBinaryData I_right( I.getPtr()+32,  32 );

   SecureBinaryData newKey;
   SecureBinaryData parFinger = extKey.getFingerprint();

   // Index list is an array of n-values needed to get from the master node to
   // this one.  The new keys should have an index list one bigger. 
   list<uint32_t> idxList = extKey.getIndicesList();
   idxList.push_back(n);

   if(extKey.hasPriv())
   {
      // This computes the private key, and lets ExtendedKey compute pub
      newKey = SecureBinaryData(CryptoECDSA().ECMultiplyScalars(I_left, extKey.getPriv()));
      return ExtendedKey().CreateFromPrivate(newKey, I_right, parFinger, idxList);
   }
   else
   {
      // Compress the output if the we received compressed input
      newKey = SecureBinaryData(CryptoECDSA().ECMultiplyPoint(I_left, extKey.getPub()));
      return ExtendedKey().CreateFromPublic(newKey, I_right, parFinger, idxList);
   }
}



ExtendedKey HDWalletCrypto::ConvertSeedToMasterKey(SecureBinaryData const & seed)
{
   SecureBinaryData constKey = SecureBinaryData("Bitcoin seed");
   SecureBinaryData bytes64 = HMAC_SHA512(constKey, seed);

   SecureBinaryData KLeft (bytes64.getPtr(),    32);
   SecureBinaryData KRight(bytes64.getPtr()+32, 32);
   return ExtendedKey().CreateFromPrivate(KLeft, KRight);
}







   /* OpenSSL code (untested)
    * Maybe one day I'll get around to switching from Crypto++ to OpenSSL
   static SecureBinaryData_1_35 sigSpace(1000);
   static uint32_t sigSize = 0;

   // Create the key object
   EC_KEY* pubKey = EC_KEY_new_by_curve_name(NID_secp256k1);

   uint8_t* pbegin = privKey.getPtr();
   d2i_ECPrivateKey(&pubKey, &pbegin, privKey.getSize());

   ECDSA_sign(0, binToSign.getPtr(), 
                 binToSign.getSize(), 
                 sigSpace.getPtr(), 
                 &sigSize, 
                 pubKey)

   EC_KEY_free(pubKey);
   return SecureBinaryData_1_35(sigSpace.getPtr(), sigSize);
   */









